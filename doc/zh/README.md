# MsgBus — 高性能无锁消息总线

[English](../en/README.md)

单进程、高性能、无锁、支持 C++20 协程的消息总线，用于模块解耦与事件驱动架构。

## 特性

- **高吞吐**：单线程发布 600 万+ QPS，多线程 500 万+ QPS
- **低延迟**：对象池优化后 publish→handler 最小延迟 ~0.3μs
- **无锁发布**：基于 Vyukov bounded MPMC 队列，publish 路径完全无锁
- **Zero-copy**：侵入式引用计数 + 对象池，消除 `shared_ptr` 开销
- **类型安全**：模板 + 运行时类型校验，同一 topic 只允许一种消息类型
- **MQTT 通配符**：`*` 匹配单层、`#` 匹配多层，灵活的 topic 路由
- **多 Dispatcher**：线程池模式，topic hash 分片，水平扩展消费能力
- **C++20 协程**：`co_await bus.async_wait<T>(topic)` 原生协程等待
- **Header-only**：仅需 include，零依赖

## 环境要求

- C++20 编译器（MSVC 19.29+, GCC 11+, Clang 14+）
- CMake 3.20+

## 快速开始

### 构建

```bash
cmake -B build -S .
cmake --build build --config Release
```

### 运行示例

```bash
./build/examples/Release/basic_example
./build/examples/Release/coroutine_example
./build/examples/Release/wildcard_example
```

### 运行测试

```bash
ctest --test-dir build --build-config Release
```

### 运行压测

```bash
./build/tests/Release/msgbus_bench
```

## 集成到你的项目

MsgBus 是 header-only 库，两种集成方式：

### 方式一：直接复制头文件

将 `include/msgbus/` 目录复制到你的项目中，添加 include 路径即可。

### 方式二：CMake add_subdirectory

```cmake
add_subdirectory(MsgBus)
target_link_libraries(your_target PRIVATE msgbus)
```

### 方式三：CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(msgbus SOURCE_DIR /path/to/MsgBus)
FetchContent_MakeAvailable(msgbus)
target_link_libraries(your_target PRIVATE msgbus)
```

## API 参考

### 头文件

```cpp
#include "msgbus/message_bus.h"     // 核心（自动包含所有依赖头文件）
#include "msgbus/topic_matcher.h"   // 仅需通配符匹配工具时单独引用
```

### 创建与启停

```cpp
// 创建总线（默认：队列容量 65536，单 dispatcher，ReturnFalse 策略）
msgbus::MessageBus bus;

// 自定义队列容量
msgbus::MessageBus bus(1048576);

// 多 dispatcher 线程池（4 个 worker + 1 个 router）
msgbus::MessageBus bus(65536, 4);

// 自动检测 CPU 核心数
msgbus::MessageBus bus(65536, 0);  // 0 = hardware_concurrency

// 指定背压策略（见下方 FullPolicy 说明）
msgbus::MessageBus bus(65536, 1, msgbus::FullPolicy::Block);

// BlockTimeout + 自定义超时
msgbus::MessageBus bus(65536, 1, msgbus::FullPolicy::BlockTimeout,
                       std::chrono::milliseconds{500});

bus.start();  // 启动 dispatcher 线程
bus.stop();   // 停止并排空队列（析构时自动调用）
```

### 背压策略（FullPolicy）

控制队列满时 `publish()` 的行为：

```cpp
enum class FullPolicy {
    ReturnFalse,   // 立即返回 false（默认，零开销）
    DropOldest,    // 丢弃最旧消息，始终成功
    DropNewest,    // 静默丢弃新消息，始终返回 true
    Block,         // 阻塞直到有空间（或 bus 停止）
    BlockTimeout,  // 阻塞至超时，超时返回 false
};
```

| 策略 | `publish()` 返回值 | 行为 |
|------|-------------------|------|
| `ReturnFalse` | 满时返回 `false` | 调用者决定重试或丢弃 |
| `DropOldest` | 始终 `true` | 淘汰队列中最旧的消息 |
| `DropNewest` | 始终 `true` | 静默丢弃新消息 |
| `Block` | 始终 `true` | 阻塞调用者直到有空间 |
| `BlockTimeout` | 超时返回 `false` | 阻塞至配置的超时时间 |

```cpp
// 示例：带 500ms 超时的阻塞发布
msgbus::MessageBus bus(1024, 1, msgbus::FullPolicy::BlockTimeout,
                       std::chrono::milliseconds{500});
bus.start();
if (!bus.publish<int>("topic", 42)) {
    // 超时 — 队列满了 500ms
}
```

### 发布消息

```cpp
// 发布任意类型消息到 topic
// 返回值取决于 FullPolicy（见上方背压策略说明）
// 内部自动使用对象池复用消息对象
bool ok = bus.publish<int>("sensor/count", 42);
bus.publish<std::string>("log/info", "system started");
bus.publish<MyStruct>("data/update", {1, 3.14, "hello"});
```

### TopicHandle（缓存发布）

对同一 topic 高频发布时，使用 `TopicHandle` 跳过每次调用的 topic 字符串解析：

```cpp
// 创建缓存句柄（仅解析一次 topic 字符串）
auto handle = bus.topic<int>("sensor/temp");

// 后续发布跳过 registry 查找 — 更快的热路径
handle.publish(42);
handle.publish(50);
handle.publish(99);
```

### 订阅消息（精确匹配）

```cpp
// 订阅 topic，返回订阅 ID（用于取消订阅）
auto id = bus.subscribe<int>("sensor/count", [](const int& value) {
    std::cout << "count = " << value << "\n";
});

// 同一 topic 可有多个订阅者
auto id2 = bus.subscribe<int>("sensor/count", [](const int& value) {
    if (value > 100) alert();
});
```

### 订阅消息（通配符）

```cpp
// '*' 匹配单层
auto id = bus.subscribe<int>("sensor/*/temp", [](const int& v) {
    std::cout << "temp = " << v << "\n";
});
// 匹配: sensor/1/temp, sensor/abc/temp
// 不匹配: sensor/1/2/temp

// '#' 匹配零层或多层（必须为最后一段）
auto id2 = bus.subscribe<std::string>("log/#", [](const std::string& msg) {
    std::cout << "log: " << msg << "\n";
});
// 匹配: log, log/info, log/error/detail

// 混合使用
auto id3 = bus.subscribe<int>("sensor/*/data/#", [](const int& v) { ... });
// 匹配: sensor/1/data, sensor/1/data/temp, sensor/abc/data/x/y
```

### 取消订阅

```cpp
bus.unsubscribe(id);  // 精确匹配和通配符订阅均可取消
```

### 协程等待

```cpp
#include <coroutine>

// 需要一个协程 Task 类型（fire-and-forget）
struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

// 在协程中等待指定 topic 的消息
Task myCoroutine(msgbus::MessageBus& bus) {
    auto msg = co_await bus.async_wait<std::string>("event/notify");
    std::cout << "received: " << msg << "\n";
}
```

## 使用示例

### 基础发布/订阅

```cpp
#include "msgbus/message_bus.h"
#include <iostream>

struct SensorData {
    int sensor_id;
    double value;
};

int main() {
    msgbus::MessageBus bus;
    bus.start();

    // 订阅
    bus.subscribe<SensorData>("sensor/temp", [](const SensorData& d) {
        std::cout << "Sensor " << d.sensor_id << ": " << d.value << "\n";
    });

    // 发布
    bus.publish<SensorData>("sensor/temp", {1, 23.5});
    bus.publish<SensorData>("sensor/temp", {2, 67.8});

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bus.stop();
}
```

### 通配符订阅

```cpp
msgbus::MessageBus bus;
bus.start();

// 订阅所有传感器数据
bus.subscribe<double>("sensor/#", [](const double& v) {
    std::cout << "any sensor: " << v << "\n";
});

// 订阅特定层级
bus.subscribe<double>("sensor/*/temp", [](const double& v) {
    std::cout << "temp: " << v << "\n";
});

bus.publish<double>("sensor/1/temp", 23.5);   // 两个 handler 都收到
bus.publish<double>("sensor/1/humid", 67.8);  // 只有 # handler 收到
```

### 多 Dispatcher 线程池

```cpp
// 4 个 worker 线程，适合高负载多 topic 场景
msgbus::MessageBus bus(65536, 4);
bus.start();

// 同一 topic 的消息保证顺序
// 不同 topic 的消息可并行处理
bus.subscribe<int>("topic/a", handler_a);
bus.subscribe<int>("topic/b", handler_b);

// topic/a 和 topic/b 可能在不同 worker 线程中并行处理
```

### 多 Topic 解耦

```cpp
// 模块 A：只发布，不关心谁在监听
void moduleA(msgbus::MessageBus& bus) {
    bus.publish<std::string>("log/error", "disk full");
    bus.publish<int>("metrics/cpu", 85);
}

// 模块 B：只订阅，不关心谁在发布
void moduleB(msgbus::MessageBus& bus) {
    bus.subscribe<std::string>("log/error", [](const std::string& msg) {
        sendAlert(msg);
    });
    bus.subscribe<int>("metrics/cpu", [](const int& usage) {
        if (usage > 90) throttle();
    });
}
```

### 协程事件等待

```cpp
Task handleShutdown(msgbus::MessageBus& bus) {
    std::cout << "Waiting for shutdown...\n";
    auto code = co_await bus.async_wait<int>("system/shutdown");
    std::cout << "Shutting down with code " << code << "\n";
    cleanup();
}

int main() {
    msgbus::MessageBus bus;
    bus.start();

    handleShutdown(bus);  // 协程立即启动，在 co_await 处挂起

    // ... 业务逻辑 ...

    bus.publish<int>("system/shutdown", 0);  // 触发协程恢复

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bus.stop();
}
```

## 注意事项

1. **类型安全**：同一 topic 只允许一种消息类型。对已有 topic 使用不同类型会抛出 `std::runtime_error`。
2. **队列满**：`publish()` 在队列满时的行为取决于 `FullPolicy`（默认 `ReturnFalse` 返回 `false`）。参见[背压策略](#背压策略fullpolicy)。
3. **Handler 线程**：单 dispatcher 模式下所有 handler 在一个线程中执行；多 dispatcher 模式下 handler 在 worker 线程中执行（同 topic 同 worker）。handler 应尽量轻量，避免阻塞。
4. **Handler 异常**：单个 handler 抛异常不会影响其他订阅者，异常被静默捕获。
5. **消息顺序**：同一 topic 的消息按 publish 顺序投递（单 dispatcher 直接保证；多 dispatcher 通过 hash 分片到同一 worker 保证）。
6. **协程安全**：`async_wait` 是一次性等待，收到一条消息后自动取消订阅。内置 `atomic<bool>` 防护多 dispatcher 下的双发 race。确保 `MessageBus` 的生命周期长于所有挂起的协程。
7. **生命周期**：确保 `MessageBus` 的生命周期长于所有订阅者和协程。在协程挂起期间销毁 bus 会导致未定义行为。
8. **通配符规则**：`*` 匹配恰好一层，`#` 匹配零层或多层且必须为 pattern 最后一段。
9. **通配符 handler 线程安全**：多 dispatcher 模式下，通配符匹配的不同 topic 可能在不同 worker 线程中并发调用同一 handler，用户需保证通配符 handler 的线程安全性。
10. **平台说明**：在支持 `std::atomic<std::shared_ptr>` 的平台（MSVC 19.28+、GCC 12+）上，dispatch 读路径完全无锁。在 Apple Clang / libc++ 上使用简短 mutex 回退（极短临界区，影响极小）。

## 构建选项

| CMake 选项 | 默认值 | 说明 |
|------------|--------|------|
| `MSGBUS_BUILD_EXAMPLES` | ON | 构建示例 |
| `MSGBUS_BUILD_TESTS` | ON | 构建测试（自动拉取 Google Test） |
| `MSGBUS_COVERAGE` | OFF | 代码覆盖率（仅 GCC/Clang，添加 `--coverage` 编译标志） |

```bash
# 仅构建库，不构建示例和测试
cmake -B build -S . -DMSGBUS_BUILD_EXAMPLES=OFF -DMSGBUS_BUILD_TESTS=OFF

# 启用代码覆盖率（Linux/macOS）
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DMSGBUS_COVERAGE=ON
cmake --build build
ctest --test-dir build
lcov --capture --directory build --output-file coverage.info
lcov --remove coverage.info '/usr/*' '*/googletest/*' --output-file coverage.info
lcov --list coverage.info

# Windows 使用 OpenCppCoverage
OpenCppCoverage --sources include -- build\tests\Debug\msgbus_tests.exe
```

## 许可证

MIT
