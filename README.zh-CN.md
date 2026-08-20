# ScalaAPI Gateway(网关)

> **本仓库已迁移。** 开发继续在
> [ScalaAPI-Platform](https://github.com/GMorandi/ScalaAPI-Platform) 仓库的
> `gateway/` 目录进行,通过保留完整历史的 subtree 合并导入(导入提交
> `8af79f0`;最终同步点为本仓库 master `3349d64`)。本仓库已归档,只读。

[English](README.md) | **简体中文**

ScalaAPI 平台的高性能 LLM API 边缘网关。基于 C++20 与
[Photon](https://github.com/alibaba/PhotonLibOS) 协程运行时构建,终结客户端协议、
执行网关侧策略,并通过 Cap'n Proto IPC 通道将每一个计费请求转发给 ScalaAPI 平台。

> 状态:积极开发中,尚未通过发布认证。请仅作为
> [ScalaAPI 超项目](https://github.com/GMorandi/ScalaAPI)配对发布的一部分部署。

## 功能

- **协议面**:OpenAI Chat Completions / Responses、Anthropic Messages、Gemini
  generateContent(JSON 与 SSE)、嵌入、模型目录、token 计数——每条流均有严格的
  终止/用量校验。
- **媒体**:图片生成(同步/异步/批量)、视频、大型 multipart 请求体。超过 512 KiB
  的请求体经分块 blob RPC 上传至平台并替换为摘要引用,保持 dispatch 帧精简。
- **实时**:WebSocket 会话(OpenAI Realtime 形态),双向帧管道,会话级上限
  (字节/帧数/时长),以及 `safe` 中止处置——策略性中止时保留结算证据而不计费。
- **Dispatch**:每个请求通过 Unix 域套接字上的 Cap'n Proto RPC 从平台获取租约 +
  余额冻结,支持幂等键、可重试故障转移、用量结算上报(token、缓存、时长、断连原因)。
- **调度**:账号/模型路由、粘性会话、RPM 限制、429 冷却,以及由平台 dispatch
  决策驱动的上游故障转移。

## 目录结构

```
src/
  server/      HTTP 服务器、网关处理器、路由、能力匹配
  protocol/    各协议解析、SSE、转换、校验
  dispatch/    Cap'n Proto dispatch 客户端(租约/用量/中止/blob RPC)
  forwarder/   上游转发、流式传输、重试
  auth/        API Key 认证与权限
  cache/       基于 Garnet 的目录/配置缓存
  platform/    平台通道管理
  usage/       用量事件采集与上报
proto/         Cap'n Proto 契约(vendored 副本;权威副本位于
               ScalaAPI-Platform/contracts/capnp,两份必须字节一致)
test/          单元测试(CTest)与基准测试
```

## 构建

需要 C++20 编译器、CMake 3.16+,首次配置需要网络(Cap'n Proto v1.0.2 与 Photon
通过 `FetchContent` 拉取)。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j"$(nproc)"
```

CMake 4.x 必须加 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`(内嵌 Cap'n Proto 所需)。
测试与基准为可选:

```sh
cmake -S . -B build -DGATEWAY_BUILD_TESTS=ON -DGATEWAY_BUILD_BENCHMARKS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

## 运行

网关设计为与平台 silo 同容器网络运行,dispatch 套接字通过卷共享。参考拓扑
(网关、两个平台 silo、PostgreSQL、Garnet、MinIO、provider mock)见 ScalaAPI-Platform
的 [`deploy/stack/`](https://github.com/GMorandi/ScalaAPI-Platform/tree/master/deploy/stack)。
主要环境变量:

| 变量 | 用途 |
| --- | --- |
| `SCALAPI_DISPATCH_SOCKET` | 平台 dispatch Unix 套接字路径 |
| `GATEWAY_PORT` / `GATEWAY_CORES` | 监听端口与 Photon vCPU 数 |
| `SCALAPI_MAX_SESSION_BYTES` / `_FRAMES` / `_DURATION_SEC` | 实时会话上限 |
| `SCALAPI_MAX_INLINE_BODY_BYTES` | blob 卸载前的内联请求体阈值 |

## 契约纪律

Cap'n Proto schema 由网关与平台共享。任何 schema 变更都必须是跨两个仓库的原子
配对变更;超项目的 `validate-pair.sh` 会拒绝契约副本不一致的配对。
