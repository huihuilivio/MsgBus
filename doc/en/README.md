# MsgBus — High-Performance Lock-Free Message Bus

[中文](../zh/README.md)

A single-process, high-performance, lock-free message bus with C++20 coroutine support, designed for module decoupling and event-driven architectures.

## Features

- **High Throughput**: 6M+ QPS single-thread publish, 5M+ QPS multi-thread
- **Low Latency**: ~0.3μs minimum publish→handler latency with object pool optimization
- **Lock-Free Publish**: Based on Vyukov bounded MPMC queue, fully lock-free publish path
- **Zero-Copy**: Intrusive reference counting + object pool, eliminates `shared_ptr` overhead
- **Type-Safe**: Template + runtime type checking, one message type per topic
- **MQTT Wildcards**: `*` matches one level, `#` matches multiple levels for flexible topic routing
- **Multi-Dispatcher**: Thread pool mode with topic hash sharding for horizontal scaling
- **C++20 Coroutines**: Native `co_await bus.async_wait<T>(topic)` support
- **Header-Only**: Just include, zero dependencies

## Requirements

- C++20 compiler (MSVC 19.29+, GCC 11+, Clang 14+)
- CMake 3.20+

## Quick Start

### Build

```bash
cmake -B build -S .
cmake --build build --config Release
```

### Run Examples

```bash
./build/examples/Release/basic_example
./build/examples/Release/coroutine_example
./build/examples/Release/wildcard_example
```

### Run Tests

```bash
ctest --test-dir build --build-config Release
```

### Run Benchmarks

```bash
./build/tests/Release/msgbus_bench
```

## Integration

MsgBus is a header-only library with two integration methods:

### Option 1: Copy Headers

Copy the `include/msgbus/` directory into your project and add the include path.

### Option 2: CMake add_subdirectory

```cmake
add_subdirectory(MsgBus)
target_link_libraries(your_target PRIVATE msgbus)
```

### Option 3: CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(msgbus SOURCE_DIR /path/to/MsgBus)
FetchContent_MakeAvailable(msgbus)
target_link_libraries(your_target PRIVATE msgbus)
```

## API Reference

### Headers

```cpp
#include "msgbus/message_bus.h"     // Core (auto-includes all dependencies)
#include "msgbus/topic_matcher.h"   // Standalone wildcard matcher utility
```

### Create & Lifecycle

```cpp
// Default: queue capacity 65536, single dispatcher, ReturnFalse policy
msgbus::MessageBus bus;

// Custom queue capacity
msgbus::MessageBus bus(1048576);

// Multi-dispatcher thread pool (4 workers + 1 router)
msgbus::MessageBus bus(65536, 4);

// Auto-detect CPU cores
msgbus::MessageBus bus(65536, 0);  // 0 = hardware_concurrency

// With backpressure policy (see FullPolicy below)
msgbus::MessageBus bus(65536, 1, msgbus::FullPolicy::Block);

// BlockTimeout with custom timeout
msgbus::MessageBus bus(65536, 1, msgbus::FullPolicy::BlockTimeout,
                       std::chrono::milliseconds{500});

bus.start();  // Start dispatcher thread(s)
bus.stop();   // Stop and drain queue (called automatically on destruction)
```

### Backpressure Policy (FullPolicy)

Controls `publish()` behavior when the internal queue is full:

```cpp
enum class FullPolicy {
    ReturnFalse,   // Return false immediately (default, zero overhead)
    DropOldest,    // Drop the oldest message; always succeeds
    DropNewest,    // Drop the new message silently; always returns true
    Block,         // Block until space is available (or bus stops)
    BlockTimeout,  // Block up to a timeout, then return false
};
```

| Policy | `publish()` returns | Behavior |
|--------|-------------------|----------|
| `ReturnFalse` | `false` if full | Caller decides retry or discard |
| `DropOldest` | always `true` | Evicts oldest queued message |
| `DropNewest` | always `true` | Silently drops the new message |
| `Block` | always `true` | Blocks caller until space available |
| `BlockTimeout` | `false` on timeout | Blocks up to the configured timeout |

```cpp
// Example: blocking publisher with 500ms timeout
msgbus::MessageBus bus(1024, 1, msgbus::FullPolicy::BlockTimeout,
                       std::chrono::milliseconds{500});
bus.start();
if (!bus.publish<int>("topic", 42)) {
    // Timed out — queue was full for 500ms
}
```

### Publish Messages

```cpp
// Publish any type to a topic.
// Return value depends on FullPolicy (see Backpressure Policy above).
// Internally uses object pool for message reuse.
bool ok = bus.publish<int>("sensor/count", 42);
bus.publish<std::string>("log/info", "system started");
bus.publish<MyStruct>("data/update", {1, 3.14, "hello"});
```

### TopicHandle (Cached Publish)

For high-frequency publishing to the same topic, use `TopicHandle` to skip topic string resolution on each call:

```cpp
// Create a cached handle (resolves topic string once)
auto handle = bus.topic<int>("sensor/temp");

// Subsequent publishes skip registry lookup — faster hot path
handle.publish(42);
handle.publish(50);
handle.publish(99);
```

### Subscribe (Exact Match)

```cpp
// Subscribe to a topic, returns subscription ID for unsubscribe
auto id = bus.subscribe<int>("sensor/count", [](const int& value) {
    std::cout << "count = " << value << "\n";
});

// Multiple subscribers on the same topic
auto id2 = bus.subscribe<int>("sensor/count", [](const int& value) {
    if (value > 100) alert();
});
```

### Subscribe (Wildcards)

```cpp
// '*' matches exactly one level
auto id = bus.subscribe<int>("sensor/*/temp", [](const int& v) {
    std::cout << "temp = " << v << "\n";
});
// Matches: sensor/1/temp, sensor/abc/temp
// No match: sensor/1/2/temp

// '#' matches zero or more levels (must be the last segment)
auto id2 = bus.subscribe<std::string>("log/#", [](const std::string& msg) {
    std::cout << "log: " << msg << "\n";
});
// Matches: log, log/info, log/error/detail

// Mixed wildcards
auto id3 = bus.subscribe<int>("sensor/*/data/#", [](const int& v) { ... });
// Matches: sensor/1/data, sensor/1/data/temp, sensor/abc/data/x/y
```

### Unsubscribe

```cpp
bus.unsubscribe(id);  // Works for both exact and wildcard subscriptions
```

### Coroutine Await

```cpp
#include <coroutine>

// Fire-and-forget coroutine task type
struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

// Wait for a message on a topic in a coroutine
Task myCoroutine(msgbus::MessageBus& bus) {
    auto msg = co_await bus.async_wait<std::string>("event/notify");
    std::cout << "received: " << msg << "\n";
}
```

## Usage Examples

### Basic Publish/Subscribe

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

    bus.subscribe<SensorData>("sensor/temp", [](const SensorData& d) {
        std::cout << "Sensor " << d.sensor_id << ": " << d.value << "\n";
    });

    bus.publish<SensorData>("sensor/temp", {1, 23.5});
    bus.publish<SensorData>("sensor/temp", {2, 67.8});

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bus.stop();
}
```

### Wildcard Subscriptions

```cpp
msgbus::MessageBus bus;
bus.start();

// Subscribe to all sensor data
bus.subscribe<double>("sensor/#", [](const double& v) {
    std::cout << "any sensor: " << v << "\n";
});

// Subscribe at a specific level
bus.subscribe<double>("sensor/*/temp", [](const double& v) {
    std::cout << "temp: " << v << "\n";
});

bus.publish<double>("sensor/1/temp", 23.5);   // Both handlers triggered
bus.publish<double>("sensor/1/humid", 67.8);  // Only # handler triggered
```

### Multi-Dispatcher Thread Pool

```cpp
// 4 worker threads, suitable for high-load multi-topic scenarios
msgbus::MessageBus bus(65536, 4);
bus.start();

// Messages on the same topic are delivered in order
// Messages on different topics may be processed in parallel
bus.subscribe<int>("topic/a", handler_a);
bus.subscribe<int>("topic/b", handler_b);

// topic/a and topic/b may be processed by different worker threads
```

### Multi-Topic Decoupling

```cpp
// Module A: only publishes, doesn't care who's listening
void moduleA(msgbus::MessageBus& bus) {
    bus.publish<std::string>("log/error", "disk full");
    bus.publish<int>("metrics/cpu", 85);
}

// Module B: only subscribes, doesn't care who's publishing
void moduleB(msgbus::MessageBus& bus) {
    bus.subscribe<std::string>("log/error", [](const std::string& msg) {
        sendAlert(msg);
    });
    bus.subscribe<int>("metrics/cpu", [](const int& usage) {
        if (usage > 90) throttle();
    });
}
```

### Coroutine Event Waiting

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

    handleShutdown(bus);  // Coroutine starts immediately, suspends at co_await

    // ... business logic ...

    bus.publish<int>("system/shutdown", 0);  // Triggers coroutine resume

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bus.stop();
}
```

## Important Notes

1. **Type Safety**: Each topic allows only one message type. Using a different type on an existing topic throws `std::runtime_error`.
2. **Queue Full**: `publish()` behavior on queue full depends on `FullPolicy` (default: `ReturnFalse` returns `false`). See [Backpressure Policy](#backpressure-policy-fullpolicy).
3. **Handler Threads**: In single-dispatcher mode, all handlers run in one thread; in multi-dispatcher mode, handlers run in worker threads (same topic → same worker). Keep handlers lightweight; avoid blocking.
4. **Handler Exceptions**: A handler throwing an exception does not affect other subscribers; exceptions are silently caught.
5. **Message Ordering**: Messages on the same topic are delivered in publish order (single-dispatcher: guaranteed directly; multi-dispatcher: guaranteed via hash sharding to the same worker).
6. **Coroutine Safety**: `async_wait` is a one-shot wait that auto-unsubscribes after receiving one message. Built-in `atomic<bool>` guard prevents double-resume under multi-dispatcher. Ensure `MessageBus` outlives all pending coroutines.
7. **Lifetime**: Ensure `MessageBus` outlives all subscribers and coroutines. Destroying the bus while coroutines are suspended leads to undefined behavior.
8. **Wildcard Rules**: `*` matches exactly one level, `#` matches zero or more trailing levels and must be the last segment.
9. **Wildcard Handler Thread Safety**: In multi-dispatcher mode, different topics matching the same wildcard pattern may invoke the handler concurrently from different worker threads. Ensure your wildcard handlers are thread-safe.
10. **Platform Note**: On platforms with `std::atomic<std::shared_ptr>` support (MSVC 19.28+, GCC 12+), the dispatch read path is fully lock-free. On Apple Clang / libc++, a brief mutex fallback is used (very short critical section, minimal impact).

## Build Options

| CMake Option | Default | Description |
|---|---|---|
| `MSGBUS_BUILD_EXAMPLES` | ON | Build examples |
| `MSGBUS_BUILD_TESTS` | ON | Build tests (auto-fetches Google Test) |
| `MSGBUS_COVERAGE` | OFF | Code coverage (GCC/Clang only, adds `--coverage` flags) |

```bash
# Build library only, no examples or tests
cmake -B build -S . -DMSGBUS_BUILD_EXAMPLES=OFF -DMSGBUS_BUILD_TESTS=OFF

# Enable code coverage (Linux/macOS)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DMSGBUS_COVERAGE=ON
cmake --build build
ctest --test-dir build
lcov --capture --directory build --output-file coverage.info
lcov --remove coverage.info '/usr/*' '*/googletest/*' --output-file coverage.info
lcov --list coverage.info

# Windows: use OpenCppCoverage
OpenCppCoverage --sources include -- build\tests\Debug\msgbus_tests.exe
```

## License

MIT
