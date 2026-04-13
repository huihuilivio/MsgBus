# MessageBus 设计文档

## 一、项目目标

实现一个 **单进程、高性能、无锁（或极低锁）、支持协程的消息总线系统**，用于模块解耦与事件驱动架构。

### 核心能力

* 单进程内高性能通信
* Topic（字符串）路由
* 异步处理（无阻塞 publish）
* 基本无锁（lock-free / wait-free）
* 支持协程（C++20）
* 支持订阅 / 取消订阅
* 支持自定义消息类型

---

## 二、总体架构

```
                ┌──────────────────────────────────────────────┐
                │              MessageBus                      │
                │                                              │
  Producer ─────┤  publish<T>(topic, msg)                      │
  Producer ─────┤       │                                      │
  Producer ─────┤       ▼                                      │
                │  ┌─────────────────────┐                     │
                │  │  Lock-Free Queue    │  MPMC Ring Buffer   │
                │  │  (Vyukov算法)       │  容量自动对齐2^N    │
                │  └────────┬────────────┘                     │
                │           │                                  │
                │           ▼                                  │
                │  ┌─────────────────────┐                     │
                │  │  Dispatcher Thread  │  单线程消费         │
                │  │  (三级退避策略)      │  spin→yield→sleep  │
                │  └────────┬────────────┘                     │
                │           │                                  │
                │           ▼  按topic路由                     │
                │  ┌──────────────────────────────────┐        │
                │  │  TopicSlot<T> (COW 订阅者列表)    │        │
                │  │  ┌────────┐ ┌────────┐ ┌───────┐ │        │
                │  │  │ Sub 1  │ │ Sub 2  │ │Sub N  │ │        │
                │  │  └────────┘ └────────┘ └───────┘ │        │
                │  └──────────────────────────────────┘        │
                │           │                                  │
                │           ▼                                  │
                │  ┌──────────────────────────────────┐        │
                │  │  handler(msg) / coroutine resume │        │
                │  └──────────────────────────────────┘        │
                └──────────────────────────────────────────────┘
```

---

## 三、项目结构

```
MsgBus/
├── CMakeLists.txt                      # 根构建文件
├── design.md                           # 设计文档（本文件）
├── README.md                           # 使用文档
├── BENCHMARK.md                        # 性能文档
├── include/
│   └── msgbus/
│       ├── lock_free_queue.h           # MPMC 无锁环形队列
│       ├── message.h                   # IMessage 类型擦除 + TypedMessage<T>
│       ├── subscriber.h               # Subscriber<T> 订阅者定义
│       ├── topic_slot.h               # TopicSlot<T> COW 订阅管理
│       └── message_bus.h              # MessageBus 核心 + 协程 Awaitable
├── examples/
│   ├── CMakeLists.txt
│   ├── basic_example.cpp              # 基础 pub/sub 示例
│   └── coroutine_example.cpp          # C++20 协程示例
└── tests/
    ├── CMakeLists.txt
    ├── test_message_bus.cpp            # 单元测试（16 个 GTest 用例）
    └── bench_message_bus.cpp           # 性能压测
```

---

## 四、核心模块设计

### 4.1 LockFreeQueue — 无锁环形队列

**文件**: `include/msgbus/lock_free_queue.h`

基于 Dmitry Vyukov 的 bounded MPMC queue 算法实现。

```cpp
template <typename T>
class LockFreeQueue {
public:
    explicit LockFreeQueue(size_t capacity);
    bool try_enqueue(T value);   // 入队，队列满返回 false
    bool try_dequeue(T& value);  // 出队，队列空返回 false
};
```

**关键设计**：

| 特性 | 实现 |
|------|------|
| 算法 | Vyukov bounded MPMC，每个 Cell 含 sequence 原子计数器 |
| 容量 | 自动向上对齐到 2^N，使用位掩码取模（避免 % 开销） |
| 内存序 | enqueue/dequeue 位置使用 `relaxed`，Cell sequence 使用 `acquire/release` |
| 缓存行 | enqueue_pos\_ 和 dequeue_pos\_ 各 `alignas(64)` 避免 false sharing |
| 线程安全 | 完全无锁，支持任意数量生产者/消费者并发 |

---

### 4.2 Message — 类型擦除消息

**文件**: `include/msgbus/message.h`

```cpp
struct IMessage {                           // 基类接口
    virtual const std::string& topic();
    virtual const std::type_info& type();
};

template <typename T>
struct TypedMessage : IMessage {            // 具体类型消息
    std::string topic_;
    T data_;
};

using MessagePtr = std::shared_ptr<IMessage>;
```

**设计要点**：

* 使用虚函数 + `typeid` 实现运行时类型安全校验
* `shared_ptr` 管理生命周期，消息在 publish→queue→dispatch→handler 全链路安全
* 模板参数 `T` 支持任意可移动类型

---

### 4.3 TopicSlot — COW 订阅管理

**文件**: `include/msgbus/topic_slot.h`

```cpp
struct ITopicSlot {
    virtual void dispatch(const MessagePtr& msg) = 0;
    virtual bool removeSubscriber(SubscriptionId id) = 0;
};

template <typename T>
class TopicSlot : public ITopicSlot { ... };
```

**Copy-on-Write 策略**：

```
subscribe/unsubscribe 时:
  1. lock(mutex)
  2. 复制当前订阅者列表 → 新 shared_ptr<vector>
  3. 修改新列表
  4. 原子替换指针
  5. unlock

dispatch 时:
  1. lock(mutex) → 拷贝 shared_ptr → unlock
  2. 遍历快照，调用 handler（无锁）
```

* **读路径（dispatch）**：只持锁拷贝一个 shared_ptr，遍历完全无锁
* **写路径（subscribe/unsubscribe）**：低频操作，mutex 保护 COW
* **异常隔离**：单个 handler 抛异常不影响其他订阅者

---

### 4.4 MessageBus — 核心总线

**文件**: `include/msgbus/message_bus.h`

```cpp
class MessageBus {
public:
    explicit MessageBus(size_t queue_capacity = 65536);
    void start();                                           // 启动 dispatcher 线程
    void stop();                                            // 停止并排空队列

    template <typename T>
    bool publish(const std::string& topic, T msg);          // 发布消息

    template <typename T, typename Handler>
    SubscriptionId subscribe(const std::string& topic, Handler&& handler);

    void unsubscribe(SubscriptionId id);                    // 取消订阅

    template <typename T>
    AsyncWaitAwaitable<T> async_wait(const std::string& topic); // 协程等待
};
```

**Topic 类型安全**：

* 每个 topic 绑定首次 subscribe/publish 的类型 `T`
* 类型不匹配时抛出 `std::runtime_error`
* 使用 `std::shared_mutex` 实现读写分离：getOrCreateSlot 读多写少

**Dispatcher 三级退避**：

```
空闲计数 < 64  → 自旋（最低延迟）
空闲计数 < 256 → yield（让出 CPU）
空闲计数 ≥ 256 → sleep 50μs（节省 CPU）
消息到达      → 立即重置计数，回到自旋
```

**优雅关闭**：stop() 设置 `running_=false` 后，dispatcher 排空队列中剩余消息再退出。

---

### 4.5 AsyncWaitAwaitable — 协程支持

```cpp
template <typename T>
class AsyncWaitAwaitable {
    // await_suspend: 注册一次性订阅，消息到达时 resume coroutine
    // await_resume: 返回消息值，自动取消订阅
};
```

**使用 SharedState 共享状态**，避免 awaitable 对象生命周期问题（MSVC 协程帧管理）。

```
co_await bus.async_wait<T>(topic)
  → await_suspend: subscribe, 保存 coroutine_handle
  → 消息到来: dispatcher 调用 handler → 存结果 → resume handle
  → await_resume: unsubscribe, 返回结果
```

---

## 五、关键设计决策

### 5.1 无锁层次

| 操作 | 锁级别 | 说明 |
|------|--------|------|
| `publish` | **完全无锁** | 仅原子 CAS 入队 |
| `subscribe` | mutex (COW) | 低频操作，复制整个列表 |
| `unsubscribe` | mutex (COW) | 同上 |
| `dispatch` | **读无锁** | 仅 shared_ptr 拷贝加锁，遍历无锁 |
| Topic 查找 | shared_mutex 读锁 | 哈希表查找，读多写少 |

### 5.2 内存管理

* **消息生命周期**：`shared_ptr<IMessage>` 从 publish 到 handler 完成后自动释放
* **订阅者列表**：`shared_ptr<vector<Subscriber>>` COW，读者持有快照不受写者影响
* **协程状态**：`shared_ptr<SharedState>` 确保 handler 回调和 awaitable 共享安全

### 5.3 类型系统

* `template<T>` 编译期类型安全
* `IMessage` + `typeid` 运行时校验
* 同一 topic 只允许一种类型，防止 `static_pointer_cast` 错误

---

## 六、性能设计

### 热路径分析

| 路径 | 复杂度 | 锁 |
|------|--------|----|
| publish → enqueue | O(1) | 无锁 |
| dequeue → dispatch | O(subscribers) | 读无锁 |
| subscribe | O(n) copy | mutex |

### 缓存友好

* 队列 enqueue/dequeue 计数器分别 64 字节对齐，避免 false sharing
* 环形队列连续内存，顺序访问

---

## 七、未来演进方向

| 方向 | 优先级 | 说明 |
|------|--------|------|
| 多线程 dispatcher | P2 | 按 topic hash 分片到线程池 |
| Backpressure | P2 | publish 返回 false 时限流策略 |
| Batch 消费 | P2 | 一次 dequeue 多条，减少调度开销 |
| Zero-copy 优化 | P3 | 避免 shared_ptr 开销，使用对象池 |
| 持久化 | P3 | 扩展为轻量 MQ |
| 跨进程 IPC | P3 | 共享内存 + 信号量 |

---

## 八、实现进度

### 基础实现
- [x] 实现 Lock-Free Queue（Vyukov bounded MPMC）
- [x] 实现 MessageBus 基础结构
- [x] 实现 publish（无锁路径）
- [x] 实现 subscribe（copy-on-write）
- [x] 实现 unsubscribe

### 分发逻辑
- [x] 实现 dispatcher 线程（三级退避）
- [x] 实现 topic 路由（shared_mutex 读写分离）
- [x] 实现 handler 调用（异常隔离）

### 协程支持
- [x] 实现 AsyncWaitAwaitable
- [x] 实现 async_wait\<T\>
- [x] 实现 coroutine resume（SharedState 安全管理）

### 可靠性
- [x] 异常隔离（handler 崩溃保护）
- [ ] 日志系统接入
- [ ] metrics（队列长度、延迟）

### 工程化
- [x] 单元测试（16 个 GTest 用例）
- [x] 性能压测（6 个基准场景）
- [x] 文档完善
- [x] 示例代码（基础 + 协程）

### 待实现
- [ ] 引入 MPMC 高性能队列（moodycamel 对比）
- [ ] 优化内存分配（对象池）
- [ ] 支持多 dispatcher（线程池）
- [ ] 支持 topic 通配符
- [ ] 支持优先级队列
- [ ] 支持延迟消息
- [ ] 支持跨进程通信（IPC）
