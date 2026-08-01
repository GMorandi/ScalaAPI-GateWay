#include "server/gateway_handler.h"
#include "platform/logging.h"
#include "platform/metrics.h"

#include <photon/thread/thread.h>
#include <xxhash.h>

#include <chrono>
#include <format>

namespace gateway::server {

GatewayHandler::GatewayHandler(cache::GarnetClient& garnet,
                               dispatch::CapnpDispatchClient& dispatch,
                               usage::UsageCollector& collector,
                               auth::SpeculativeCache& auth_cache)
    : garnet_(garnet),
      dispatch_(dispatch),
      collector_(collector),
      auth_cache_(auth_cache),
      api_key_auth_(auth_cache),
      forwarder_(forwarder::Forwarder::create({})) {}

int GatewayHandler::handle(const HttpRequest& req, HttpResponse& resp,
                           dispatch::DispatchRequest::EndpointKind endpoint) {
    auto& metrics = platform::global_metrics();
    metrics.requests_total.fetch_add(1, std::memory_order_relaxed);
    metrics.active_connections.fetch_add(1, std::memory_order_relaxed);

    auto start = std::chrono::steady_clock::now();
    std::string request_id = std::format("{:016x}", XXH64(&start, sizeof(start), 0));

    // --- Step 1: Extract API key and authenticate ---
    auto raw_key = extract_api_key(req);
    if (raw_key.empty()) {
        resp.status_code = 401;
        metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
        metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
        return 0;
    }

    auto key_hash = auth::ApiKeyAuth::hash_key(raw_key);

    // --- Step 2: Garnet hot-path auth lookup (sub-μs) ---
    std::string garnet_key = std::format("auth:{}", key_hash);
    auto garnet_resp = garnet_.get(garnet_key);

    int64_t cached_version = 0;
    if (garnet_resp.found) {
        metrics.garnet_hits.fetch_add(1, std::memory_order_relaxed);
        cached_version = 1;
    } else {
        metrics.garnet_misses.fetch_add(1, std::memory_order_relaxed);
    }

    // --- Step 3: Parse request body ---
    protocol::Format inbound_format;
    switch (endpoint) {
        case dispatch::DispatchRequest::EndpointKind::Messages:
            inbound_format = protocol::Format::Anthropic;
            break;
        case dispatch::DispatchRequest::EndpointKind::ChatCompletions:
            inbound_format = protocol::Format::OpenAIChatCompletions;
            break;
        case dispatch::DispatchRequest::EndpointKind::Responses:
            inbound_format = protocol::Format::OpenAIResponses;
            break;
        default:
            inbound_format = protocol::Format::Gemini;
            break;
    }

    auto parsed = protocol::Converter::parse(req.body, inbound_format);
    bool is_stream = parsed.stream;

    if (is_stream) {
        metrics.requests_streaming.fetch_add(1, std::memory_order_relaxed);
    }

    // --- Step 4: Compute session hash for sticky scheduling ---
    auto session_hash = compute_session_hash(req.body, parsed.model);

    // --- Step 5: Cap'n Proto Dispatch (auth + schedule + slot acquire) ---
    dispatch::DispatchRequest dispatch_req{
        .api_key_hash = key_hash,
        .requested_model = parsed.model,
        .session_hash = session_hash,
        .client_ip = std::string(req.client_ip),
        .request_id = request_id,
        .excluded_accounts = {},
        .cached_auth_version = cached_version,
        .endpoint = static_cast<int>(endpoint),
        .metadata_user_id = parsed.metadata_user_id,
    };

    forwarder::FailoverController failover;
    dispatch::DispatchResult dispatch_result;

    while (true) {
        dispatch_result = dispatch_.dispatch(dispatch_req);
        metrics.dispatch_calls.fetch_add(1, std::memory_order_relaxed);

        if (dispatch_result.outcome == dispatch::DispatchResult::Outcome::Ok) {
            break;
        }

        if (dispatch_result.outcome == dispatch::DispatchResult::Outcome::Reauth) {
            break;
        }

        if (dispatch_result.outcome == dispatch::DispatchResult::Outcome::Rejected) {
            resp.status_code = 429;
            metrics.requests_failed.fetch_add(1, std::memory_order_relaxed);
            metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }

        if (dispatch_result.outcome == dispatch::DispatchResult::Outcome::Wait) {
            photon::thread_usleep(
                static_cast<uint64_t>(dispatch_result.wait_timeout_ms) * 1000);
            continue;
        }
    }

    // --- Step 6: Forward to upstream ---
    auto& target = dispatch_result.upstream;

    protocol::Format upstream_format;
    if (target.platform == "anthropic" || target.platform == "claude")
        upstream_format = protocol::Format::Anthropic;
    else if (target.platform == "gemini" || target.platform == "google")
        upstream_format = protocol::Format::Gemini;
    else
        upstream_format = protocol::Format::OpenAIChatCompletions;

    std::string upstream_body = protocol::Converter::convert_request(
        req.body, inbound_format, upstream_format, target.mapped_model);

    auto forward_result = forwarder_->forward(
        target, upstream_body, is_stream, resp.stream_write);

    // --- Step 7: Handle upstream errors with failover ---
    if (forward_result.status_code >= 400 && forward_result.status_code != 400) {
        auto action = failover.handle_error(target.account_id, forward_result.status_code);

        if (action == forwarder::FailoverController::Action::SwitchAccount ||
            action == forwarder::FailoverController::Action::Continue) {

            dispatch::ErrorReportData err{
                .account_id = target.account_id,
                .status_code = forward_result.status_code,
                .retry_after_ms = 0,
                .request_id = request_id,
            };
            dispatch_.report_upstream_error(err);
            metrics.upstream_errors.fetch_add(1, std::memory_order_relaxed);

            dispatch_req.excluded_accounts.assign(
                failover.failed_accounts().begin(),
                failover.failed_accounts().end());
            metrics.failovers.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // --- Step 8: Report usage (fire-and-forget) ---
    auto elapsed = std::chrono::steady_clock::now() - start;
    usage::UsageEvent event{
        .lease_token = dispatch_result.lease_token,
        .request_id = request_id,
        .api_key_id = 0,
        .user_id = target.user_id,
        .account_id = target.account_id,
        .group_id = target.group_id,
        .model = parsed.model,
        .upstream_model = target.mapped_model,
        .input_tokens = forward_result.input_tokens,
        .output_tokens = forward_result.output_tokens,
        .cache_create_tokens = forward_result.cache_create_tokens,
        .cache_read_tokens = forward_result.cache_read_tokens,
        .duration_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()),
        .first_token_ms = forward_result.first_token_ms,
        .stream = is_stream,
        .client_disconnect = forward_result.client_disconnect,
    };
    collector_.record(std::move(event));

    resp.status_code = forward_result.status_code > 0 ? forward_result.status_code : 200;
    metrics.active_connections.fetch_sub(1, std::memory_order_relaxed);
    return 0;
}

std::string GatewayHandler::extract_api_key(const HttpRequest& req) {
    if (req.authorization.starts_with("Bearer ")) {
        return std::string(req.authorization.substr(7));
    }
    if (!req.x_api_key.empty()) {
        return std::string(req.x_api_key);
    }
    return "";
}

std::string GatewayHandler::compute_session_hash(std::string_view body,
                                                  std::string_view model) {
    auto hash = XXH64(model.data(), model.size(), 0);
    return std::format("{:016x}", hash);
}

}  // namespace gateway::server
