#include "auth/api_key_auth.h"
#include "platform/logging.h"

#include <openssl/sha.h>
#include <format>

namespace gateway::auth {

ApiKeyAuth::ApiKeyAuth(SpeculativeCache& cache) : cache_(cache) {}

std::string ApiKeyAuth::hash_key(std::string_view raw_key) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(raw_key.data()),
           raw_key.size(), hash);
    std::string hex;
    hex.reserve(SHA256_DIGEST_LENGTH * 2);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        hex += std::format("{:02x}", hash[i]);
    }
    return hex;
}

std::optional<AuthSnapshot> ApiKeyAuth::authenticate(
    std::string_view raw_key, std::string_view client_ip) {
    auto key_hash = hash_key(raw_key);
    return cache_.lookup(key_hash);
}

}  // namespace gateway::auth
