# ScalaAPI Gateway

> **This repository has moved.** Development continues in
> [ScalaAPI-Platform](https://github.com/GMorandi/ScalaAPI-Platform) under
> `gateway/`, imported via a history-preserving subtree merge (import commit
> `8af79f0`; final synchronized source: master `3349d64` of this repository).
> This repository is archived and read-only.

**English** | [简体中文](README.zh-CN.md)

High-performance LLM API edge gateway for the ScalaAPI platform. Built in C++20
on the [Photon](https://github.com/alibaba/PhotonLibOS) coroutine runtime, it
terminates client protocols, enforces gateway-side policy, and forwards every
billable request to the ScalaAPI platform over a Cap'n Proto IPC channel.

> Status: active development, not yet release-certified. Deploy only as part of
> the paired release described in the [ScalaAPI superproject](https://github.com/GMorandi/ScalaAPI).

## What it does

- **Protocol surface**: OpenAI Chat Completions / Responses, Anthropic Messages,
  Gemini generateContent (JSON and SSE), embeddings, model catalogue, token
  counting — with strict terminal/usage validation on every stream.
- **Media**: image generation (sync/async/batch), video, and large multipart
  bodies. Bodies over 512 KiB are uploaded to the platform through a chunked
  blob RPC and replaced by a digest reference, keeping the dispatch frame small.
- **Realtime**: WebSocket sessions (OpenAI Realtime shape) with bidirectional
  frame piping, session-level caps (bytes / frames / duration), and a `safe`
  abort disposition that settles evidence without charging on strategic aborts.
- **Dispatch**: every request acquires a lease + balance hold from the platform
  via Cap'n Proto RPC over a Unix domain socket, with idempotency keys,
  retry-aware failover, and usage settlement reporting (tokens, cache, duration,
  disconnect reasons).
- **Scheduling**: account/model routing, sticky sessions, RPM limits, 429
  cooldown, and upstream failover driven by platform dispatch decisions.

## Layout

```
src/
  server/      HTTP server, gateway handler, routing, capability matching
  protocol/    Per-protocol parsing, SSE, conversions, validation
  dispatch/    Cap'n Proto dispatch client (lease/usage/abort/blob RPC)
  forwarder/   Upstream forwarding, streaming, retries
  auth/        API key authentication and scopes
  cache/       Garnet-backed catalogue/config cache
  platform/    Platform channel management
  usage/       Usage event collection and reporting
proto/         Cap'n Proto contract (vendored copy; canonical copy lives in
               ScalaAPI-Platform/contracts/capnp and must stay byte-identical)
test/          Unit tests (CTest) and benchmarks
```

## Build

Requires a C++20 compiler, CMake 3.16+, and network access on first configure
(Cap'n Proto v1.0.2 and Photon are fetched by `FetchContent`).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j"$(nproc)"
```

On CMake 4.x the `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` flag is required by the
bundled Cap'n Proto build. Tests and benchmarks are optional:

```sh
cmake -S . -B build -DGATEWAY_BUILD_TESTS=ON -DGATEWAY_BUILD_BENCHMARKS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

## Running

The gateway is designed to run as a container beside the platform silos, with
the dispatch socket shared through a volume. See
[`deploy/stack/`](https://github.com/GMorandi/ScalaAPI-Platform/tree/master/deploy/stack)
in ScalaAPI-Platform for the reference Compose topology (gateway, two platform
silos, PostgreSQL, Garnet, MinIO, provider mock). Key environment variables:

| Variable | Purpose |
| --- | --- |
| `SCALAPI_DISPATCH_SOCKET` | Platform dispatch Unix socket path |
| `GATEWAY_PORT` / `GATEWAY_CORES` | Listen port and Photon vCPU count |
| `SCALAPI_MAX_SESSION_BYTES` / `_FRAMES` / `_DURATION_SEC` | Realtime session caps |
| `SCALAPI_MAX_INLINE_BODY_BYTES` | Inline body threshold before blob offload |

## Contract discipline

The Cap'n Proto schema is shared between gateway and platform. Any schema change
must be an atomic paired change across both repositories; `validate-pair.sh` in
the superproject rejects pairs whose contract copies diverge.
