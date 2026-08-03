#include <photon/common/alog.h>
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARN
#undef LOG_ERROR
#include "platform/logging.h"
#include <spdlog/sinks/stdout_color_sinks.h>

namespace gateway::platform {

void init_logging() {
    auto logger = spdlog::stdout_color_mt("gateway");
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [%t] %v");
    set_log_output_level(ALOG_INFO);
}

}  // namespace gateway::platform
