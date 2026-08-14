#include "cache/garnet_client.h"
#include "platform/logging.h"

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <photon/thread/thread.h>
#include <openssl/ssl.h>

#include <cstring>
#include <format>
#include <mutex>

namespace gateway::cache {

struct GarnetClient::Impl {
    std::string host;
    uint16_t port = 6379;
    std::string password;
    bool use_tls = false;
    std::string server_name;
    std::string ca_cert_path;
    int fd = -1;
    SSL_CTX* tls_context = nullptr;
    SSL* tls = nullptr;
    photon::mutex mutex;
    std::string accum;
    char read_buf[64 * 1024];

    void disconnect() {
        if (tls) {
            SSL_shutdown(tls);
            SSL_free(tls);
        }
        if (tls_context) SSL_CTX_free(tls_context);
        tls = nullptr;
        tls_context = nullptr;
        if (fd >= 0) ::close(fd);
        fd = -1;
        accum.clear();
    }

    bool ensure_connected() {
        if (fd >= 0) return true;
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        const auto port_text = std::to_string(port);
        addrinfo* addresses = nullptr;
        if (::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &addresses) != 0)
            return false;

        for (auto* address = addresses; address != nullptr; address = address->ai_next) {
            fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (fd >= 0 && ::connect(fd, address->ai_addr, address->ai_addrlen) == 0)
                break;
            disconnect();
        }
        ::freeaddrinfo(addresses);
        if (fd < 0) return false;

        timeval timeout{3, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (use_tls && !start_tls()) {
            disconnect();
            return false;
        }
        if (!password.empty()) {
            auto auth = std::format("*2\r\n$4\r\nAUTH\r\n${}\r\n{}\r\n",
                                    password.size(), password);
            if (!send_command(auth) || !read_response().starts_with("+OK")) {
                disconnect();
                return false;
            }
        }
        LOG_INFO("Connected to Garnet at {}:{} tls={}", host, port, use_tls);
        return true;
    }

    bool start_tls() {
        tls_context = SSL_CTX_new(TLS_client_method());
        if (!tls_context) return false;
        SSL_CTX_set_verify(tls_context, SSL_VERIFY_PEER, nullptr);
        const auto trust_loaded = ca_cert_path.empty()
            ? SSL_CTX_set_default_verify_paths(tls_context)
            : SSL_CTX_load_verify_locations(tls_context, ca_cert_path.c_str(), nullptr);
        if (trust_loaded != 1) return false;

        tls = SSL_new(tls_context);
        if (!tls) return false;
        const auto& expected_name = server_name.empty() ? host : server_name;
        if (SSL_set_fd(tls, fd) != 1 ||
            SSL_set_tlsext_host_name(tls, expected_name.c_str()) != 1 ||
            SSL_set1_host(tls, expected_name.c_str()) != 1 ||
            SSL_connect(tls) != 1) {
            return false;
        }
        return true;
    }

    ssize_t write_bytes(const char* data, size_t size) {
        if (tls) return SSL_write(tls, data, static_cast<int>(size));
        return ::send(fd, data, size, MSG_NOSIGNAL);
    }

    ssize_t read_bytes(char* data, size_t size) {
        if (tls) return SSL_read(tls, data, static_cast<int>(size));
        return ::read(fd, data, size);
    }

    bool send_command(std::string_view cmd) {
        if (!ensure_connected()) return false;
        size_t total = 0;
        while (total < cmd.size()) {
            ssize_t n = write_bytes(cmd.data() + total, cmd.size() - total);
            if (n <= 0) return false;
            total += n;
        }
        return true;
    }

    bool fill_until(size_t need) {
        while (accum.size() < need) {
            ssize_t n = read_bytes(read_buf, sizeof(read_buf));
            if (n <= 0) return false;
            accum.append(read_buf, n);
        }
        return true;
    }

    bool fill_until_crlf(size_t start = 0) {
        while (accum.find("\r\n", start) == std::string::npos) {
            ssize_t n = read_bytes(read_buf, sizeof(read_buf));
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

std::unique_ptr<GarnetClient> GarnetClient::connect(const std::string& host,
                                                    uint16_t port,
                                                    const std::string& password,
                                                    bool use_tls,
                                                    const std::string& server_name,
                                                    const std::string& ca_cert_path) {
    auto client = std::make_unique<GarnetClient>();
    client->impl_ = std::make_unique<Impl>();
    client->impl_->host = host;
    client->impl_->port = port;
    client->impl_->password = password;
    client->impl_->use_tls = use_tls;
    client->impl_->server_name = server_name;
    client->impl_->ca_cert_path = ca_cert_path;

    if (!client->impl_->ensure_connected()) {
        LOG_ERROR("Failed to connect to Garnet at {}:{}", host, port);
    }
    return client;
}

GarnetClient::~GarnetClient() {
    if (impl_) impl_->disconnect();
}

GarnetResponse GarnetClient::get(std::string_view key) {
    auto cmd = std::format("*2\r\n$3\r\nGET\r\n${}\r\n{}\r\n",
                           key.size(), key);
    auto raw = impl_->execute(cmd);
    if (raw.empty()) return {.found = false, .error = true};  // Connection/command failure

    if (raw[0] == '$') {
        if (raw.size() >= 3 && raw[1] == '-') {
            return {.found = false, .error = false};  // $-1 = nil (key doesn't exist)
        }
        auto crlf = raw.find("\r\n");
        if (crlf == std::string::npos) return {.found = false, .error = true};
        auto data = raw.substr(crlf + 2);
        if (data.ends_with("\r\n")) data = data.substr(0, data.size() - 2);
        return {.found = true, .error = false, .value = std::move(data)};
    }
    return {.found = false, .error = true};  // Unexpected response format
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
