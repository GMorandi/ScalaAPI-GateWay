#include "cache/garnet_client.h"
#include "platform/logging.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <photon/thread/thread.h>

#include <cstring>
#include <format>
#include <mutex>

namespace gateway::cache {

struct GarnetClient::Impl {
    std::string uds_path;
    int fd = -1;
    photon::mutex mutex;
    std::string accum;
    char read_buf[64 * 1024];

    void disconnect() {
        if (fd >= 0) ::close(fd);
        fd = -1;
        accum.clear();
    }

    bool ensure_connected() {
        if (fd >= 0) return true;
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, uds_path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            disconnect();
            return false;
        }
        timeval timeout{3, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        LOG_INFO("Connected to Garnet at {}", uds_path);
        return true;
    }

    bool send_command(std::string_view cmd) {
        if (!ensure_connected()) return false;
        size_t total = 0;
        while (total < cmd.size()) {
            ssize_t n = ::send(fd, cmd.data() + total, cmd.size() - total, MSG_NOSIGNAL);
            if (n <= 0) return false;
            total += n;
        }
        return true;
    }

    bool fill_until(size_t need) {
        while (accum.size() < need) {
            ssize_t n = ::read(fd, read_buf, sizeof(read_buf));
            if (n <= 0) return false;
            accum.append(read_buf, n);
        }
        return true;
    }

    bool fill_until_crlf(size_t start = 0) {
        while (accum.find("\r\n", start) == std::string::npos) {
            ssize_t n = ::read(fd, read_buf, sizeof(read_buf));
            if (n <= 0) return false;
            accum.append(read_buf, n);
        }
        return true;
    }

    std::string read_response() {
        if (fd < 0) return "";
        accum.clear();

        if (!fill_until_crlf()) return "";

        if (accum[0] == '$') {
            auto crlf = accum.find("\r\n");
            auto len_str = accum.substr(1, crlf - 1);
            int64_t len = std::atoll(len_str.c_str());
            if (len < 0) {
                accum.erase(0, crlf + 2);
                return "$-1\r\n";
            }
            size_t total_needed = crlf + 2 + len + 2;
            if (!fill_until(total_needed)) return "";
            auto result = accum.substr(0, total_needed);
            accum.erase(0, total_needed);
            return result;
        }

        auto crlf = accum.find("\r\n");
        auto result = accum.substr(0, crlf + 2);
        accum.erase(0, crlf + 2);
        return result;
    }

    std::string execute(std::string_view command) {
        std::lock_guard<photon::mutex> guard(mutex);
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (send_command(command)) {
                auto response = read_response();
                if (!response.empty()) return response;
            }
            disconnect();
        }
        return {};
    }
};

std::unique_ptr<GarnetClient> GarnetClient::connect(const std::string& uds_path) {
    auto client = std::make_unique<GarnetClient>();
    client->impl_ = std::make_unique<Impl>();
    client->impl_->uds_path = uds_path;

    if (!client->impl_->ensure_connected()) {
        LOG_ERROR("Failed to connect to Garnet at {}", uds_path);
    }
    return client;
}

GarnetClient::~GarnetClient() {
    if (impl_ && impl_->fd >= 0) {
        ::close(impl_->fd);
    }
}

GarnetResponse GarnetClient::get(std::string_view key) {
    auto cmd = std::format("*2\r\n$3\r\nGET\r\n${}\r\n{}\r\n",
                           key.size(), key);
    auto raw = impl_->execute(cmd);
    if (raw.empty()) return {};

    if (raw[0] == '$') {
        if (raw.size() >= 3 && raw[1] == '-') {
            return {};  // $-1 = nil
        }
        auto crlf = raw.find("\r\n");
        if (crlf == std::string::npos) return {};
        auto data = raw.substr(crlf + 2);
        if (data.ends_with("\r\n")) data = data.substr(0, data.size() - 2);
        return {.found = true, .value = std::move(data)};
    }
    return {};
}

std::vector<GarnetResponse> GarnetClient::mget(std::vector<std::string_view> keys) {
    std::vector<GarnetResponse> results;
    results.reserve(keys.size());
    for (auto& k : keys) {
        results.push_back(get(k));
    }
    return results;
}

bool GarnetClient::ping() {
    auto resp = impl_->execute("*1\r\n$4\r\nPING\r\n");
    return resp.starts_with("+PONG");
}

}  // namespace gateway::cache
