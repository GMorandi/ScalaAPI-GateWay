#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace gateway::cache {

struct GarnetResponse {
    bool found = false;
    bool error = false;
    std::string value;
};

class GarnetClient {
public:
    static std::unique_ptr<GarnetClient> connect(const std::string& host,
                                                 uint16_t port,
                                                 const std::string& password = {},
                                                 bool use_tls = false,
                                                 const std::string& server_name = {},
                                                 const std::string& ca_cert_path = {});
    ~GarnetClient();

    GarnetResponse get(std::string_view key);
    std::vector<GarnetResponse> mget(std::vector<std::string_view> keys);
    bool ping();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::cache
