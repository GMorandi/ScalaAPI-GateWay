#include "forwarder/forwarder.h"
#include "forwarder/connection_pool.h"
#include "forwarder/stream_pipe.h"
#include "platform/logging.h"

#include <photon/net/http/client.h>
#include <photon/net/http/message.h>
#include <photon/thread/thread.h>

#include <chrono>
#include <cstring>

namespace gateway::forwarder {

struct Forwarder::Impl {
    ForwardConfig config;
    std::unique_ptr<ConnectionPool> pool;
};

std::unique_ptr<Forwarder> Forwarder::create(const ForwardConfig& config) {
    auto f = std::make_unique<Forwarder>();
    f->impl_ = std::make_unique<Impl>();
    f->impl_->config = config;
    f->impl_->pool = ConnectionPool::create(64);
    return f;
}

Forwarder::~Forwarder() = default;

static int extract_usage_int(std::string_view body, std::string_view key) {
    auto pos = body.find(key);
    if (pos == std::string_view::npos) return 0;
    pos = body.find(':', pos + key.size());
    if (pos == std::string_view::npos) return 0;
    ++pos;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
    int val = 0;
    while (pos < body.size() && body[pos] >= '0' && body[pos] <= '9') {
        val = val * 10 + (body[pos] - '0');
        ++pos;
    }
    return val;
}

ForwardResult Forwarder::forward(const dispatch::UpstreamTarget& target,
                                  std::string_view body,
                                  bool stream,
                                  StreamWriteFn stream_write) {
    ForwardResult result;

    std::string url = target.base_url;
    if (!target.upstream_path.empty()) {
        if (!url.empty() && url.back() != '/' && target.upstream_path.front() != '/')
            url += '/';
        url += target.upstream_path;
    }

    if (url.empty()) {
        result.status_code = 502;
        result.error = "empty upstream URL";
        return result;
    }

    auto* http_client = impl_->pool->get_client(target.base_url);
    if (!http_client) {
        result.status_code = 502;
        result.error = "HTTP client not available";
        return result;
    }

    auto* op = http_client->new_operation(
        photon::net::http::Verb::POST, url);

    op->timeout = {impl_->config.total_stream_timeout_ms * 1000ULL};

    for (auto& [key, value] : target.auth_headers) {
        op->req.headers.insert(key, value);
    }

    op->req.headers.insert("Content-Type", "application/json");
    op->set_body(body);

    if (!target.proxy_url.empty()) {
        op->set_proxy(target.proxy_url);
    }

    auto start = std::chrono::steady_clock::now();
    int ret = op->call();

    if (ret < 0) {
        result.status_code = 502;
        result.error = "upstream connection failed";
        LOG_ERROR("Forward to {} failed: {}", url, strerror(errno));
        http_client->destroy_operation(op);
        return result;
    }

    result.status_code = op->resp.status_code();

    if (!stream) {
        std::string resp_body;
        char buf[64 * 1024];
        while (true) {
            ssize_t n = op->resp.read(buf, sizeof(buf));
            if (n <= 0) break;
            resp_body.append(buf, n);
        }

        auto elapsed = std::chrono::steady_clock::now() - start;
        result.duration_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

        if (resp_body.find("\"usage\"") != std::string::npos) {
            result.input_tokens = extract_usage_int(resp_body, "\"input_tokens\"");
            result.output_tokens = extract_usage_int(resp_body, "\"output_tokens\"");
            result.cache_create_tokens = extract_usage_int(resp_body, "\"cache_creation_input_tokens\"");
            result.cache_read_tokens = extract_usage_int(resp_body, "\"cache_read_input_tokens\"");
            if (result.input_tokens == 0)
                result.input_tokens = extract_usage_int(resp_body, "\"prompt_tokens\"");
            if (result.output_tokens == 0)
                result.output_tokens = extract_usage_int(resp_body, "\"completion_tokens\"");
        }

        result.stream = false;
    } else {
        StreamPipeConfig pipe_cfg{
            .read_buf_size = impl_->config.read_buf_size,
            .write_buf_size = impl_->config.write_buf_size,
            .first_token_timeout_ms = impl_->config.first_token_timeout_ms,
            .inter_chunk_timeout_ms = impl_->config.inter_chunk_timeout_ms,
            .total_timeout_ms = impl_->config.total_stream_timeout_ms,
            .keepalive_interval_ms = impl_->config.keepalive_interval_ms,
            .inject_keepalive = true,
        };

        StreamPipe pipe(pipe_cfg, ProtocolMode::Passthrough);

        auto upstream_read = [&](char* buf, size_t len) -> ssize_t {
            return op->resp.read(buf, len);
        };

        std::string accumulated;
        auto client_write = [&](const char* data, size_t len) -> ssize_t {
            if (stream_write) {
                return stream_write(data, len);
            }
            accumulated.append(data, len);
            return static_cast<ssize_t>(len);
        };

        auto stream_result = pipe.run(upstream_read, client_write);

        result.stream = true;
        result.input_tokens = stream_result.input_tokens;
        result.output_tokens = stream_result.output_tokens;
        result.cache_create_tokens = stream_result.cache_create_tokens;
        result.cache_read_tokens = stream_result.cache_read_tokens;
        result.first_token_ms = stream_result.first_token_ms;
        result.duration_ms = stream_result.total_duration_ms;
        result.client_disconnect = stream_result.client_disconnect;
    }

    http_client->destroy_operation(op);
    return result;
}

}  // namespace gateway::forwarder
