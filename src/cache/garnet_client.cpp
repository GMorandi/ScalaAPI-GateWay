#include "cache/garnet_client.h"
#include "platform/logging.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <format>

namespace gateway::cache {

struct GarnetClient::Impl {
    std::string uds_path;
    int fd = -1;
    char read_buf[64 * 1024];

    bool send_command(std::string_view cmd) {
        if (fd < 0) return false;
        size_t total = 0;
        while (total < cmd.size()) {
            ssize_t n = ::write(fd, cmd.data() + total, cmd.size() - total);
            if (n <= 0) return false;
            total += n;
        }
        return true;
    }

    std::string read_response() {
        if (fd < 0) return "";
        ssize_t n = ::read(fd, read_buf, sizeof(read_buf));
        if (n <= 0) return "";
        return std::string(read_buf, n);
    }
};

std::unique_ptr<GarnetClient> GarnetClient::connect(const std::string& uds_path) {
    auto client = std::make_unique<GarnetClient>();
    client->impl_ = std::make_unique<Impl>();
    client->impl_->uds_path = uds_path;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("Failed to create unix socket for Garnet");
        return nullptr;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, uds_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("Failed to connect to Garnet at {}", uds_path);
        ::close(fd);
        return nullptr;
    }

    client->impl_->fd = fd;
    LOG_INFO("Connected to Garnet at {}", uds_path);
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
    if (!impl_->send_command(cmd)) {
        return {};
    }

    auto raw = impl_->read_response();
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
    return impl_->send_command("*1\r\n$4\r\nPING\r\n");
}

}  // namespace gateway::cache
