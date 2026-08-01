FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y \
    build-essential cmake git libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --parallel $(nproc)

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y libssl3 && rm -rf /var/lib/apt/lists/*
RUN mkdir -p /var/run/sub2api
COPY --from=build /src/build/gateway /usr/local/bin/gateway
EXPOSE 8080
ENTRYPOINT ["gateway"]
