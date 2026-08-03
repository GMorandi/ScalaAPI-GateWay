FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y \
    build-essential cmake git libaio-dev libssl-dev libsqlite3-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
        -DGATEWAY_BUILD_TESTS=OFF \
        -DGATEWAY_BUILD_BENCHMARKS=OFF && \
    cmake --build build --parallel $(nproc)

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y libaio1t64 libssl3 libsqlite3-0 curl \
    && rm -rf /var/lib/apt/lists/*
RUN mkdir -p /var/run/sub2api /var/lib/sub2api
COPY --from=build /src/build/gateway /usr/local/bin/gateway
EXPOSE 8080
ENTRYPOINT ["gateway"]
