#include "platform/fault_injection.h"
#include "platform/logging.h"

#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace gateway::platform {
namespace {

std::string sanitize(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (const auto character : value)
        output.push_back((character >= 'a' && character <= 'z')
                         || (character >= 'A' && character <= 'Z')
                         || (character >= '0' && character <= '9')
            ? character : '-');
    return output;
}

bool enabled(std::string_view value) {
    return value == "1" || value == "true" || value == "TRUE";
}

}  // namespace

bool FaultInjection::claim(std::string_view point, std::string_view correlation) {
    const auto* configured = std::getenv("GATEWAY_FAULT_HOOK");
    if (!configured || configured != point)
        return false;

    if (enabled(std::getenv("GATEWAY_FAULT_REPEAT") ?: "false"))
        return true;

    const auto* configured_directory = std::getenv("GATEWAY_FAULT_MARKER_DIR");
    const std::string directory = configured_directory && *configured_directory
        ? configured_directory : "/tmp/scalaapi-fault-hooks";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;

    const auto path = directory + "/" + sanitize(point) + ".claimed";
    const auto fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) return false;
    const auto content = std::string("point=") + std::string(point)
        + "\ncorrelation=" + std::string(correlation)
        + "\npid=" + std::to_string(static_cast<long long>(::getpid())) + "\n";
    (void)::write(fd, content.data(), content.size());
    (void)::fsync(fd);
    (void)::close(fd);
    return true;
}

void FaultInjection::crash_if_configured(std::string_view point,
                                         std::string_view correlation) {
    if (!claim(point, correlation)) return;
    LOG_ERROR("Fault injection claimed point {} for {}; terminating process",
              point, correlation);
    std::abort();
}

}  // namespace gateway::platform
