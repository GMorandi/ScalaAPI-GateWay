#include "usage/usage_collector.h"
#include "platform/logging.h"

#include <deque>
#include <stdexcept>

extern "C" {
struct sqlite3;
struct sqlite3_stmt;
int sqlite3_open_v2(const char*, sqlite3**, int, const char*);
int sqlite3_close(sqlite3*);
int sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
const char* sqlite3_errmsg(sqlite3*);
void sqlite3_free(void*);
int sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int sqlite3_step(sqlite3_stmt*);
int sqlite3_finalize(sqlite3_stmt*);
int sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int sqlite3_bind_int(sqlite3_stmt*, int, int);
int sqlite3_bind_int64(sqlite3_stmt*, int, long long);
long long sqlite3_column_int64(sqlite3_stmt*, int);
int sqlite3_column_int(sqlite3_stmt*, int);
const unsigned char* sqlite3_column_text(sqlite3_stmt*, int);
}

namespace gateway::usage {
namespace {
constexpr int SQLITE_OK = 0;
constexpr int SQLITE_ROW = 100;
constexpr int SQLITE_DONE = 101;
constexpr int SQLITE_OPEN_READWRITE = 0x00000002;
constexpr int SQLITE_OPEN_CREATE = 0x00000004;
constexpr int SQLITE_OPEN_FULLMUTEX = 0x00010000;
auto SQLITE_TRANSIENT = reinterpret_cast<void (*)(void*)>(-1);

void execute(sqlite3* db, const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : sqlite3_errmsg(db);
        if (error) sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void add_column_if_missing(sqlite3* db, const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) == SQLITE_OK) return;
    std::string message = error ? error : sqlite3_errmsg(db);
    if (error) sqlite3_free(error);
    // SQLite has no ADD COLUMN IF NOT EXISTS. A duplicate is the expected
    // outcome for an already-migrated durable outbox; all other failures are
    // actionable and must not be hidden.
    if (message.find("duplicate column name") == std::string::npos)
        throw std::runtime_error(message);
}

std::string text_column(sqlite3_stmt* statement, int column) {
    auto* value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : "";
}
}

struct UsageCollector::Impl {
    sqlite3* db = nullptr;
    std::deque<UsageEvent> memory;
};

UsageCollector::UsageCollector(std::string database_path)
    : impl_(std::make_unique<Impl>()) {
    if (database_path.empty()) return;
    if (sqlite3_open_v2(database_path.c_str(), &impl_->db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr) != SQLITE_OK) {
        auto message = impl_->db ? sqlite3_errmsg(impl_->db) : "sqlite open failed";
        throw std::runtime_error(message);
    }
    execute(impl_->db, "PRAGMA journal_mode=WAL");
    execute(impl_->db, "PRAGMA synchronous=FULL");
    execute(impl_->db, "PRAGMA busy_timeout=5000");
    execute(impl_->db, R"SQL(
        CREATE TABLE IF NOT EXISTS usage_outbox (
            lease_token TEXT PRIMARY KEY,
            request_id TEXT NOT NULL,
            api_key_id INTEGER NOT NULL,
            user_id INTEGER NOT NULL,
            account_id INTEGER NOT NULL,
            group_id INTEGER NOT NULL,
            model TEXT NOT NULL,
            upstream_model TEXT NOT NULL,
            input_tokens INTEGER NOT NULL,
            output_tokens INTEGER NOT NULL,
            cache_create_tokens INTEGER NOT NULL,
            cache_read_tokens INTEGER NOT NULL,
            duration_ms INTEGER NOT NULL,
            first_token_ms INTEGER NOT NULL,
            stream INTEGER NOT NULL,
            client_disconnect INTEGER NOT NULL,
            status_code INTEGER NOT NULL,
            input_image_count INTEGER NOT NULL DEFAULT 0,
            output_image_count INTEGER NOT NULL DEFAULT 0,
            image_size TEXT NOT NULL DEFAULT '',
            video_count INTEGER NOT NULL DEFAULT 0,
            video_resolution TEXT NOT NULL DEFAULT '',
            video_duration_seconds INTEGER NOT NULL DEFAULT 0,
            realtime_duration_ms INTEGER NOT NULL DEFAULT 0,
            realtime_frames INTEGER NOT NULL DEFAULT 0,
            disconnect_reason TEXT NOT NULL DEFAULT '',
            provider_usage_json TEXT NOT NULL DEFAULT '',
            reasoning_tokens INTEGER NOT NULL DEFAULT 0,
            service_tier TEXT NOT NULL DEFAULT '',
            upstream_endpoint TEXT NOT NULL DEFAULT '',
            cancellation_reason TEXT NOT NULL DEFAULT '',
            media_operation_id TEXT NOT NULL DEFAULT '',
            pricing_version TEXT NOT NULL DEFAULT '',
            response_status_code INTEGER NOT NULL DEFAULT 0,
            response_content_type TEXT NOT NULL DEFAULT '',
            response_body TEXT NOT NULL DEFAULT '',
            created_at INTEGER NOT NULL DEFAULT (unixepoch())
        )
    )SQL");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN input_image_count INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN output_image_count INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN image_size TEXT NOT NULL DEFAULT ''");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN video_count INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN video_resolution TEXT NOT NULL DEFAULT ''");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN video_duration_seconds INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN realtime_duration_ms INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN realtime_frames INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN disconnect_reason TEXT NOT NULL DEFAULT ''");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN provider_usage_json TEXT NOT NULL DEFAULT ''");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN reasoning_tokens INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN service_tier TEXT NOT NULL DEFAULT ''");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN upstream_endpoint TEXT NOT NULL DEFAULT ''");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN cancellation_reason TEXT NOT NULL DEFAULT ''");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN media_operation_id TEXT NOT NULL DEFAULT ''");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN pricing_version TEXT NOT NULL DEFAULT ''");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN response_status_code INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN response_content_type TEXT NOT NULL DEFAULT ''");
    add_column_if_missing(impl_->db, "ALTER TABLE usage_outbox ADD COLUMN response_body TEXT NOT NULL DEFAULT ''");
}

UsageCollector::~UsageCollector() {
    if (impl_ && impl_->db) sqlite3_close(impl_->db);
}

void UsageCollector::record(UsageEvent event) {
    if (!impl_->db) {
        impl_->memory.push_back(std::move(event));
        return;
    }

    constexpr const char* sql = R"SQL(
        INSERT INTO usage_outbox (
            lease_token, request_id, api_key_id, user_id, account_id, group_id,
            model, upstream_model, input_tokens, output_tokens, cache_create_tokens,
            cache_read_tokens, duration_ms, first_token_ms, stream,
            client_disconnect, status_code, input_image_count, output_image_count,
            image_size, video_count, video_resolution, video_duration_seconds,
            realtime_duration_ms, realtime_frames, disconnect_reason,
            provider_usage_json, reasoning_tokens, service_tier, upstream_endpoint,
            cancellation_reason, media_operation_id, pricing_version,
            response_status_code, response_content_type, response_body)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(lease_token) DO NOTHING
    )SQL";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(impl_->db));

    int i = 1;
    auto bind_text = [&](const std::string& value) {
        sqlite3_bind_text(statement, i++, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    };
    bind_text(event.lease_token);
    bind_text(event.request_id);
    sqlite3_bind_int64(statement, i++, event.api_key_id);
    sqlite3_bind_int64(statement, i++, event.user_id);
    sqlite3_bind_int64(statement, i++, event.account_id);
    sqlite3_bind_int64(statement, i++, event.group_id);
    bind_text(event.model);
    bind_text(event.upstream_model);
    sqlite3_bind_int(statement, i++, event.input_tokens);
    sqlite3_bind_int(statement, i++, event.output_tokens);
    sqlite3_bind_int(statement, i++, event.cache_create_tokens);
    sqlite3_bind_int(statement, i++, event.cache_read_tokens);
    sqlite3_bind_int(statement, i++, event.duration_ms);
    sqlite3_bind_int(statement, i++, event.first_token_ms);
    sqlite3_bind_int(statement, i++, event.stream ? 1 : 0);
    sqlite3_bind_int(statement, i++, event.client_disconnect ? 1 : 0);
    sqlite3_bind_int(statement, i++, event.status_code);
    sqlite3_bind_int(statement, i++, event.input_image_count);
    sqlite3_bind_int(statement, i++, event.output_image_count);
    bind_text(event.image_size);
    sqlite3_bind_int(statement, i++, event.video_count);
    bind_text(event.video_resolution);
    sqlite3_bind_int(statement, i++, event.video_duration_seconds);
    sqlite3_bind_int(statement, i++, event.realtime_duration_ms);
    sqlite3_bind_int(statement, i++, event.realtime_frames);
    bind_text(event.disconnect_reason);
    bind_text(event.provider_usage_json);
    sqlite3_bind_int(statement, i++, event.reasoning_tokens);
    bind_text(event.service_tier);
    bind_text(event.upstream_endpoint);
    bind_text(event.cancellation_reason);
    bind_text(event.media_operation_id);
    bind_text(event.pricing_version);
    sqlite3_bind_int(statement, i++, event.response_status_code);
    bind_text(event.response_content_type);
    bind_text(event.response_body);

    auto result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(impl_->db));
}

std::vector<UsageEvent> UsageCollector::peek(size_t limit) {
    if (!impl_->db) {
        auto count = std::min(limit, impl_->memory.size());
        return {impl_->memory.begin(), impl_->memory.begin() + count};
    }

    constexpr const char* sql = R"SQL(
        SELECT lease_token, request_id, api_key_id, user_id, account_id, group_id,
               model, upstream_model, input_tokens, output_tokens,
               cache_create_tokens, cache_read_tokens, duration_ms, first_token_ms,
               stream, client_disconnect, status_code, input_image_count,
               output_image_count, image_size, video_count, video_resolution,
               video_duration_seconds, realtime_duration_ms, realtime_frames,
               disconnect_reason, provider_usage_json, reasoning_tokens,
               service_tier, upstream_endpoint, cancellation_reason,
               media_operation_id, pricing_version, response_status_code,
               response_content_type, response_body
        FROM usage_outbox ORDER BY created_at, rowid LIMIT ?
    )SQL";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(impl_->db));
    sqlite3_bind_int(statement, 1, static_cast<int>(limit));

    std::vector<UsageEvent> events;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        UsageEvent event;
        event.lease_token = text_column(statement, 0);
        event.request_id = text_column(statement, 1);
        event.api_key_id = sqlite3_column_int64(statement, 2);
        event.user_id = sqlite3_column_int64(statement, 3);
        event.account_id = sqlite3_column_int64(statement, 4);
        event.group_id = sqlite3_column_int64(statement, 5);
        event.model = text_column(statement, 6);
        event.upstream_model = text_column(statement, 7);
        event.input_tokens = sqlite3_column_int(statement, 8);
        event.output_tokens = sqlite3_column_int(statement, 9);
        event.cache_create_tokens = sqlite3_column_int(statement, 10);
        event.cache_read_tokens = sqlite3_column_int(statement, 11);
        event.duration_ms = sqlite3_column_int(statement, 12);
        event.first_token_ms = sqlite3_column_int(statement, 13);
        event.stream = sqlite3_column_int(statement, 14) != 0;
        event.client_disconnect = sqlite3_column_int(statement, 15) != 0;
        event.status_code = sqlite3_column_int(statement, 16);
        event.input_image_count = sqlite3_column_int(statement, 17);
        event.output_image_count = sqlite3_column_int(statement, 18);
        event.image_size = text_column(statement, 19);
        event.video_count = sqlite3_column_int(statement, 20);
        event.video_resolution = text_column(statement, 21);
        event.video_duration_seconds = sqlite3_column_int(statement, 22);
        event.realtime_duration_ms = sqlite3_column_int(statement, 23);
        event.realtime_frames = sqlite3_column_int(statement, 24);
        event.disconnect_reason = text_column(statement, 25);
        event.provider_usage_json = text_column(statement, 26);
        event.reasoning_tokens = sqlite3_column_int(statement, 27);
        event.service_tier = text_column(statement, 28);
        event.upstream_endpoint = text_column(statement, 29);
        event.cancellation_reason = text_column(statement, 30);
        event.media_operation_id = text_column(statement, 31);
        event.pricing_version = text_column(statement, 32);
        event.response_status_code = sqlite3_column_int(statement, 33);
        event.response_content_type = text_column(statement, 34);
        event.response_body = text_column(statement, 35);
        events.push_back(std::move(event));
    }
    sqlite3_finalize(statement);
    return events;
}

void UsageCollector::acknowledge(const std::string& lease_token) {
    if (!impl_->db) {
        if (!impl_->memory.empty() && impl_->memory.front().lease_token == lease_token)
            impl_->memory.pop_front();
        return;
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(impl_->db,
            "DELETE FROM usage_outbox WHERE lease_token = ?", -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(impl_->db));
    sqlite3_bind_text(statement, 1, lease_token.c_str(),
        static_cast<int>(lease_token.size()), SQLITE_TRANSIENT);
    auto result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(impl_->db));
}

std::vector<UsageEvent> UsageCollector::drain() {
    if (!impl_->db) {
        std::vector<UsageEvent> events;
        events.reserve(impl_->memory.size());
        while (!impl_->memory.empty()) {
            events.push_back(std::move(impl_->memory.front()));
            impl_->memory.pop_front();
        }
        return events;
    }
    auto events = peek(static_cast<size_t>(-1));
    for (const auto& event : events) acknowledge(event.lease_token);
    return events;
}

size_t UsageCollector::pending() const {
    if (!impl_->db) return impl_->memory.size();
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(impl_->db,
            "SELECT COUNT(*) FROM usage_outbox", -1, &statement, nullptr) != SQLITE_OK)
        return 0;
    size_t count = sqlite3_step(statement) == SQLITE_ROW
        ? static_cast<size_t>(sqlite3_column_int64(statement, 0)) : 0;
    sqlite3_finalize(statement);
    return count;
}

bool UsageCollector::durable() const {
    return impl_->db != nullptr;
}

}  // namespace gateway::usage
