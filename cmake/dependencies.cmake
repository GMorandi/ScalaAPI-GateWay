include(FetchContent)

# GCC 16 strict-aliasing fix for PhotonLibOS
add_compile_options(-fno-strict-aliasing -Wno-error=strict-aliasing)

# PhotonLibOS
FetchContent_Declare(
    photon
    GIT_REPOSITORY https://github.com/alibaba/PhotonLibOS.git
    GIT_TAG 4dd457013c48d17c571fd6d2aa87199ae4c25d4f
    # This historical commit is not advertised by the upstream refs. A shallow
    # clone cannot resolve it, while a full fetch can still pin the exact source.
    GIT_SHALLOW FALSE
)
set(PHOTON_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(PHOTON_ENABLE_URING OFF CACHE BOOL "" FORCE)
set(PHOTON_ENABLE_LIBCURL OFF CACHE BOOL "" FORCE)
set(PHOTON_CXX_STANDARD 17 CACHE STRING "" FORCE)
FetchContent_MakeAvailable(photon)

# Cap'n Proto
FetchContent_Declare(
    capnproto
    GIT_REPOSITORY https://github.com/capnproto/capnproto.git
    GIT_TAG v1.0.2
    GIT_SHALLOW TRUE
)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(capnproto)

# simdjson
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG v3.9.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(simdjson)

# rapidjson
FetchContent_Declare(
    rapidjson
    GIT_REPOSITORY https://github.com/Tencent/rapidjson.git
    GIT_TAG 24b5e7a8b27f42fa16b96fc70aade9106cf7102f
    GIT_SHALLOW TRUE
)
set(RAPIDJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RAPIDJSON_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RAPIDJSON_BUILD_DOC OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rapidjson)

# spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(spdlog)

# xxHash
FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG v0.8.2
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR cmake_unofficial
)
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(xxhash)

# OpenSSL (system)
find_package(OpenSSL 3.0 REQUIRED)

if(GATEWAY_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

if(GATEWAY_BUILD_BENCHMARKS)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.1
        GIT_SHALLOW TRUE
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)
endif()
