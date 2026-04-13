# MsgBus — High-Performance Lock-Free Message Bus

**[English](doc/en/README.md)** | **[中文](doc/zh/README.md)**

A single-process, high-performance, lock-free message bus with C++20 coroutine support, designed for module decoupling and event-driven architectures.

单进程、高性能、无锁、支持 C++20 协程的消息总线，用于模块解耦与事件驱动架构。

## Features / 特性

- **High Throughput / 高吞吐**: 6M+ QPS single-thread, 5M+ QPS multi-thread
- **Low Latency / 低延迟**: ~0.3μs min publish→handler latency (object pool optimized)
- **Lock-Free Publish / 无锁发布**: Vyukov bounded MPMC queue
- **Zero-Copy**: Intrusive ref counting + object pool, no `shared_ptr` overhead
- **Type-Safe / 类型安全**: Template + runtime type checking, one type per topic
- **MQTT Wildcards / 通配符**: `*` one-level, `#` multi-level topic routing
- **Multi-Dispatcher / 多分发器**: Thread pool, topic hash sharding
- **C++20 Coroutines / 协程**: `co_await bus.async_wait<T>(topic)`
- **Header-Only**: Zero dependencies

## Quick Start / 快速开始

```bash
cmake -B build -S .
cmake --build build --config Release
ctest --test-dir build --build-config Release
```

## Documentation / 文档

| Document | English | 中文 |
|---|---|---|
| User Guide | [README](doc/en/README.md) | [README](doc/zh/README.md) |
| Design Doc | [design.md](doc/en/design.md) | [design.md](doc/zh/design.md) |
| Benchmarks | [BENCHMARK.md](doc/en/BENCHMARK.md) | [BENCHMARK.md](doc/zh/BENCHMARK.md) |

## License

MIT
