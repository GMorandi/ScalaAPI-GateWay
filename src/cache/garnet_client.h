#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace gateway::cache {

struct GarnetResponse {
    bool found = false;
    std::string value;
};

class GarnetClient {
public:
    static std::unique_ptr<GarnetClient> connect(const std::string& uds_path);
    ~GarnetClient();

    GarnetResponse get(std::string_view key);
    std::vector<GarnetResponse> mget(std::vector<std::string_view> keys);
    bool ping();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway::cache
