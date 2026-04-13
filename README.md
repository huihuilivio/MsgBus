# MsgBus — 高性能无锁消息总线

单进程、高性能、无锁、支持 C++20 协程的消息总线，用于模块解耦与事件驱动架构。

## 特性

- **高吞吐**：单线程发布 600 万+ QPS，多线程 500 万+ QPS
- **低延迟**：publish→handler p50 ≈ 3.6μs
- **无锁发布**：基于 Vyukov bounded MPMC 队列，publish 路径完全无锁
- **类型安全**：模板 + 运行时类型校验，同一 topic 只允许一种消息类型
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
#include "msgbus/message_bus.h"
```

### 创建与启停

```cpp
// 创建总线，可选指定队列容量（默认 65536）
msgbus::MessageBus bus;
msgbus::MessageBus bus(1048576);  // 100 万容量

bus.start();  // 启动 dispatcher 线程
bus.stop();   // 停止并排空队列（析构时自动调用）
```

### 发布消息

```cpp
// 发布任意类型消息到 topic，返回 false 表示队列已满
bool ok = bus.publish<int>("sensor/count", 42);
bus.publish<std::string>("log/info", "system started");
bus.publish<MyStruct>("data/update", {1, 3.14, "hello"});
```

### 订阅消息

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

### 取消订阅

```cpp
bus.unsubscribe(id);
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
2. **队列满**：`publish()` 在队列满时返回 `false`，调用者需重试或丢弃。
3. **Handler 线程**：所有 handler 在 dispatcher 线程中执行。handler 应尽量轻量，避免阻塞。
4. **Handler 异常**：单个 handler 抛异常不会影响其他订阅者，异常被静默捕获。
5. **消息顺序**：同一 topic 的消息按 publish 顺序投递（单 dispatcher 线程保证）。
6. **协程安全**：`async_wait` 是一次性等待，收到一条消息后自动取消订阅。
7. **生命周期**：确保 `MessageBus` 的生命周期长于所有订阅者和协程。

## 构建选项

| CMake 选项 | 默认值 | 说明 |
|------------|--------|------|
| `MSGBUS_BUILD_EXAMPLES` | ON | 构建示例 |
| `MSGBUS_BUILD_TESTS` | ON | 构建测试（自动拉取 Google Test） |

```bash
# 仅构建库，不构建示例和测试
cmake -B build -S . -DMSGBUS_BUILD_EXAMPLES=OFF -DMSGBUS_BUILD_TESTS=OFF
```

## 许可证

MIT
