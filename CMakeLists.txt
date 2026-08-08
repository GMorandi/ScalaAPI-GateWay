cmake_minimum_required(VERSION 3.20)
project(scalaapi-gateway VERSION 1.0.0 LANGUAGES CXX)

find_library(SQLITE3_LIBRARY NAMES sqlite3 libsqlite3.so.0 REQUIRED)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GATEWAY_BUILD_TESTS "Build tests" ON)
option(GATEWAY_BUILD_BENCHMARKS "Build benchmarks" OFF)

# --- Dependencies ---
include(cmake/dependencies.cmake)

# --- Cap'n Proto schema compilation ---
file(GLOB CAPNP_SCHEMAS ${CMAKE_SOURCE_DIR}/proto/*.capnp)
set(CAPNP_OUTPUT_DIR ${CMAKE_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${CAPNP_OUTPUT_DIR})

set(CAPNP_GENERATED_SRCS "")
set(CAPNP_GENERATED_HDRS "")
foreach(schema ${CAPNP_SCHEMAS})
    get_filename_component(schema_name ${schema} NAME_WE)
    list(APPEND CAPNP_GENERATED_SRCS ${CAPNP_OUTPUT_DIR}/${schema_name}.capnp.c++)
    list(APPEND CAPNP_GENERATED_HDRS ${CAPNP_OUTPUT_DIR}/${schema_name}.capnp.h)
endforeach()

set(CAPNP_EXECUTABLE ${CMAKE_BINARY_DIR}/_deps/capnproto-build/c++/src/capnp/capnp)
set(CAPNPC_CXX ${CMAKE_BINARY_DIR}/_deps/capnproto-build/c++/src/capnp/capnpc-c++)

add_custom_command(
    OUTPUT ${CAPNP_GENERATED_SRCS} ${CAPNP_GENERATED_HDRS}
    COMMAND ${CAPNP_EXECUTABLE} compile --src-prefix=${CMAKE_SOURCE_DIR}/proto
            -o ${CAPNPC_CXX}:${CAPNP_OUTPUT_DIR}
            ${CAPNP_SCHEMAS}
    DEPENDS ${CAPNP_SCHEMAS} capnp_tool capnpc_cpp
    COMMENT "Compiling Cap'n Proto schemas"
)

add_library(capnp_gen STATIC ${CAPNP_GENERATED_SRCS})
target_include_directories(capnp_gen PUBLIC ${CAPNP_OUTPUT_DIR})
target_link_libraries(capnp_gen PUBLIC capnp-rpc)

# --- Gateway core library ---
add_library(gateway_core STATIC
    src/server/http_server.cpp
    src/server/capability_registry.cpp
    src/server/router.cpp
    src/server/websocket.cpp
    src/server/gateway_handler.cpp
    src/auth/api_key_auth.cpp
    src/auth/speculative_cache.cpp
    src/auth/invalidation_subscriber.cpp
    src/protocol/converter.cpp
    src/protocol/anthropic_format.cpp
    src/protocol/openai_format.cpp
    src/protocol/openai_responses_format.cpp
    src/protocol/gemini_format.cpp
    src/forwarder/forwarder.cpp
    src/forwarder/connection_pool.cpp
    src/forwarder/retry_policy.cpp
    src/forwarder/failover.cpp
    src/forwarder/stream_pipe.cpp
    src/dispatch/capnp_dispatch_client.cpp
    src/cache/garnet_client.cpp
    src/usage/usage_collector.cpp
    src/usage/usage_reporter.cpp
    src/platform/core_runtime.cpp
    src/platform/fault_injection.cpp
    src/platform/metrics.cpp
    src/platform/logging.cpp
)

target_include_directories(gateway_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CAPNP_OUTPUT_DIR}
    ${rapidjson_SOURCE_DIR}/include
)

target_link_libraries(gateway_core PUBLIC
    photon_static
    capnp_gen
    capnp-rpc
    simdjson
    RapidJSON
    spdlog::spdlog
    xxhash
    OpenSSL::SSL
    OpenSSL::Crypto
    ${SQLITE3_LIBRARY}
)

# --- Main executable ---
add_executable(gateway src/main.cpp)
target_link_libraries(gateway PRIVATE gateway_core)

# --- Tests ---
if(GATEWAY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
