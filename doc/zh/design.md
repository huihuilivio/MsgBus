# MessageBus 设计文档

[English](../en/design.md)

## 一、项目目标

实现一个 **单进程、高性能、无锁（或极低锁）、支持协程的消息总线系统**，用于模块解耦与事件驱动架构。

### 核心能力

* 单进程内高性能通信
* Topic（字符串）路由 + MQTT 风格通配符
* 异步处理（无阻塞 publish）
* 基本无锁（lock-free / wait-free）
* 支持协程（C++20）
* 支持订阅 / 取消订阅
* 支持自定义消息类型
* Zero-copy 对象池 + 侵入式引用计数
* 多 dispatcher 线程池（topic hash 分片）

---

## 二、总体架构

```
                ┌───────────────────────────────────────────────────────────┐
                │                      MessageBus                          │
                │                                                          │
  Producer ─────┤  publish<T>(topic, msg)                                  │
  Producer ─────┤       │  ① 对象池获取/新建 TypedMessage                   │
  Producer ─────┤       │  ② 设置 recycler 回调                            │
                │       ▼                                                  │
                │  ┌─────────────────────────┐                             │
                │  │  Lock-Free Queue         │  MPMC Ring Buffer          │
                │  │  (Vyukov算法)            │  容量自动对齐2^N           │
                │  └────────┬────────────────┘                             │
                │           │                                              │
                │     ┌─────┴─────────────────────────────────┐            │
                │     │ num_dispatchers == 1?                  │            │
                │     │                                        │            │
                │  ┌──┴──────────────┐    ┌───────────────────┴──────────┐ │
                │  │ 单线程 Dispatch  │    │ 多线程 Dispatcher（线程池）   │ │
                │  │ dispatchLoop()  │    │                              │ │
                │  │ 三级退避策略     │    │  Router 线程                 │ │
                │  │ spin→yield      │    │  hash(topic) % N → 工作队列  │ │
                │  │ →sleep          │    │                              │ │
                │  └──────┬──────────┘    │  Worker[0] Worker[1] ... [N] │ │
                │         │               │  各自独立 dispatch           │ │
                │         │               └──────────┬───────────────────┘ │
                │         └────────┬─────────────────┘                     │
                │                  ▼  按topic路由                          │
                │  ┌────────────────────────────────────────┐              │
                │  │  ① 精确匹配: TopicSlot<T> (COW 列表)   │              │
                │  │  ② 通配符匹配: WildcardTrie 索引        │              │
                │  │     '*' 匹配单层 / '#' 匹配多层        │              │
                │  │  ┌────────┐ ┌────────┐ ┌───────┐      │              │
                │  │  │ Sub 1  │ │ Sub 2  │ │Sub N  │      │              │
                │  │  └────────┘ └────────┘ └───────┘      │              │
                │  └────────────────────────────────────────┘              │
                │                  │                                        │
                │                  ▼                                        │
                │  ┌────────────────────────────────────────┐              │
                │  │  handler(msg) / coroutine resume        │              │
                │  └────────────────────────────────────────┘              │
                │                  │                                        │
                │                  ▼  引用计数归零时                         │
                │  ┌────────────────────────────────────────┐              │
                │  │  ObjectPool<TypedMessage<T>> 回收复用   │              │
                │  └────────────────────────────────────────┘              │
                └───────────────────────────────────────────────────────────┘
```

---

## 三、项目结构

```
MsgBus/
├── CMakeLists.txt                      # 根构建文件
├── doc/
│   ├── en/                             # 英文文档
│   │   ├── README.md
│   │   ├── design.md
│   │   └── BENCHMARK.md
│   └── zh/                             # 中文文档
│       ├── README.md
│       ├── design.md                   # 设计文档（本文件）
│       └── BENCHMARK.md
├── .gitignore
├── include/
│   └── msgbus/
│       ├── lock_free_queue.h           # MPMC 无锁环形队列
│       ├── message.h                   # IMessage 侵入式引用计数 + TypedMessage<T> + MessagePtr
│       ├── object_pool.h              # 无锁对象池（freelist 回收）
│       ├── subscriber.h               # Subscriber<T> 订阅者定义
│       ├── topic_matcher.h            # MQTT 风格 topic 通配符匹配
│       ├── topic_registry.h           # TopicId ↔ 字符串 线程安全注册表
│       ├── topic_slot.h               # TopicSlot<T> COW 订阅管理
│       ├── wildcard_trie.h            # 通配符 Trie 索引（O(depth) 匹配）
│       └── message_bus.h              # MessageBus 核心 + 对象池 + 多dispatcher + 通配符 + 协程 + TopicRegistry
├── examples/
│   ├── CMakeLists.txt
│   ├── basic_example.cpp              # 基础 pub/sub 示例
│   ├── coroutine_example.cpp          # C++20 协程示例
│   └── wildcard_example.cpp           # 通配符订阅示例
├── tests/
│   ├── CMakeLists.txt
│   ├── test_message_bus.cpp            # 单元测试（80 个 GTest 用例）
│   └── bench_message_bus.cpp           # 性能压测（8 个基准场景）
└── .github/
    └── workflows/
        ├── ci.yml                      # CI: UT on push/PR (Windows/Linux/macOS)
        └── release.yml                 # Release: tag 预发布 + 压测 + 源码包
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

**复用场景**：主队列、多 dispatcher 工作队列、对象池 freelist 均复用此队列。

---

### 4.2 ObjectPool — 无锁对象池

**文件**: `include/msgbus/object_pool.h`

基于 LockFreeQueue 实现的无锁 freelist 对象池，用于消息对象回收复用。

```cpp
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(size_t capacity);   // freelist 最大容量
    T* acquire();                           // 获取已回收对象，空则 nullptr
    void release(T* p);                     // 归还对象，池满则 delete
};
```

**设计要点**：
* 不预分配对象，首次使用后缓存回收
* `acquire()` 和 `release()` 均为无锁操作（底层 `LockFreeQueue<T*>`）
* 池满时 `release()` 直接 `delete`，避免无限增长
* 析构时排空 freelist 并 delete 所有缓存对象

---

### 4.3 Message — 侵入式引用计数消息

**文件**: `include/msgbus/message.h`

```cpp
/// 紧凑的 topic 标识符 — 替代热路径上的 std::string
using TopicId = uint32_t;
inline constexpr TopicId kInvalidTopicId = 0;

struct IMessage {
    std::atomic<int> ref_count_{0};         // 侵入式引用计数
    void (*recycler_)(IMessage*) = nullptr; // 回收函数指针

    virtual TopicId topic_id();
    virtual const std::type_info& type();
    void add_ref() noexcept;
    bool release_ref() noexcept;            // 归零返回 true
};

template <typename T>
struct TypedMessage : IMessage {
    TopicId topic_id_;
    T data_;
    void reset(TopicId topic_id, T data); // 池复用时重置（整数赋值替代字符串拷贝）
};
```

**MessagePtr — 侵入式智能指针**：

```cpp
class MessagePtr {
public:
    static MessagePtr adopt(IMessage* p) noexcept;  // 接管指针并 add_ref
    // 拷贝/移动语义，析构时 release_ref
    // 引用计数归零时：有 recycler_ → 调用回收; 否则 delete
};
```

**设计要点**：

* **替代 `shared_ptr`**：消除独立 control block 的堆分配，引用计数直接嵌入 IMessage
* **recycler 函数指针**：publish 时设置，引用计数归零时调用，将对象归还对象池而非 delete
* **`reset()` 方法**：池复用时重置 topic_id/data/ref_count/recycler，整数赋值替代字符串拷贝
* 性能提升：最小延迟从 ~36μs 降至 ~0.6μs

---

### 4.4 TypedMessagePool — 每类型静态对象池

**文件**: `include/msgbus/message_bus.h`（内嵌）

```cpp
// 可调参数（命名常量，非魔数）
inline constexpr size_t kDefaultQueueCapacity  = 65536;
inline constexpr size_t kDefaultPoolCapacity    = 8192;
inline constexpr unsigned kSpinThreshold        = 64;
inline constexpr unsigned kYieldThreshold       = 256;

template <typename T>
struct TypedMessagePool {
    static ObjectPool<TypedMessage<T>>& instance();   // 静态单例，容量 kDefaultPoolCapacity
    static void recycle(IMessage* msg);                // 回收回调
};
```

**Publish 路径**：

```
publish<T>(topic, msg)
  → registry_.resolve(topic) → TopicId  // 字符串→整数（shared_lock 快速路径）
  → pool.acquire()
  → 命中: reset(topic_id, msg)    // 复用，整数赋值，无堆分配
  → 未命中: new TypedMessage<T>   // 首次创建
  → raw->recycler_ = &recycle     // 设置回收回调
  → queue_.try_enqueue(MessagePtr::adopt(raw))
```

**回收路径**：

```
MessagePtr 引用计数归零
  → recycler_(ptr)
  → TypedMessagePool<T>::recycle(msg)
  → pool.release(static_cast<TypedMessage<T>*>(msg))
  → 池未满: 缓存复用 / 池满: delete
```

---

### 4.5 TopicRegistry — Topic 字符串 ↔ ID 注册表

**文件**: `include/msgbus/topic_registry.h`

线程安全的 topic 字符串与整数 ID 双向映射。所有热路径（dispatch、route）使用 `TopicId`（`uint32_t`）代替 `std::string` 进行哈希和比较。

```cpp
class TopicRegistry {
public:
    TopicId resolve(std::string_view topic);      // 注册或查找 topic → 稳定 ID
    std::string_view to_string(TopicId id) const; // 反查 ID → 字符串（如通配符匹配时）
};
```

**关键设计**：

| 特性 | 实现 |
|------|------|
| 读路径 | `shared_lock` + 透明哈希查找（`string_view` 查 `string` 键，零分配） |
| 写路径 | `unique_lock` + double-check，仅首次注册时触发 |
| ID 稳定性 | 递增分配，`kInvalidTopicId = 0`，合法 ID 从 1 开始 |
| 字符串生命周期 | `id_to_sv_` 存储的 `string_view` 指向 `unordered_map` 键（迭代器稳定性保证） |

**收益**：
* dispatch 路径 `slots_` 和 `topic_types_` 从 `unordered_map<string, ...>` 变为 `unordered_map<uint32_t, ...>`
* 路由 `routeToWorker` 使用 `hash<TopicId>` 代替 `hash<string>`
* `TypedMessage<T>` 仅携带 4 字节 `TopicId` 而非 `std::string`
* 多 Topic QPS 提升 ~26%，p50 延迟降低 ~6x

---

### 4.6 TopicMatcher — MQTT 风格通配符

**文件**: `include/msgbus/topic_matcher.h`

```cpp
bool topicMatches(std::string_view pattern, std::string_view topic);
bool isWildcard(std::string_view pattern);
```

**通配符规则**（MQTT 风格）：

| 通配符 | 语义 | 示例 |
|--------|------|------|
| `*` | 匹配恰好一层 | `sensor/*/temp` 匹配 `sensor/1/temp`，不匹配 `sensor/1/2/temp` |
| `#` | 匹配零层或多层（必须为最后一段） | `sensor/#` 匹配 `sensor`、`sensor/temp`、`sensor/1/temp` |

**边界处理**：
* `sensor/#` 匹配 `sensor`（零层，`#` 匹配空）
* `a/*/c/#` 匹配 `a/x/c`（`#` 匹配零层）
* 纯精确匹配不含 `*` 或 `#` 时走快速路径（直接哈希查找）

---

### 4.7 WildcardTrie — 通配符 Trie 索引

**文件**: `include/msgbus/wildcard_trie.h`

将通配符订阅 pattern 按 topic 层级建立 Trie 树，dispatch 时沿 concrete topic 的层级遍历 Trie，复杂度为 **O(topic 层级深度)** 而非 O(N)。

```cpp
class WildcardTrie {
public:
    void insert(std::string_view pattern,  // 插入通配符 pattern
               const Entry& entry);
    bool remove(SubscriptionId id);        // 按 ID 移除（自动修剪空节点）
    void match(std::string_view topic,     // 查找匹配的 slot
               const std::type_info& type,
               std::vector<ITopicSlot*>& out) const;
    bool empty() const;                    // O(1) entry 计数器
};
```

**匹配算法**：

```
matchNode(node, levels, depth):
  1. 检查 node 的 '#' 子节点 → 匹配所有剩余层级（终端）
  2. depth == levels.size() → 检查当前节点的 entries
  3. 检查精确匹配子节点 levels[depth] → 递归 depth+1
  4. 检查 '*' 子节点 → 递归 depth+1（跳过一层）
```

**关键设计**：

| 特性 | 实现 |
|------|------|
| 复杂度 | O(topic depth)，与 pattern 数量无关 |
| 扩展性 | 100 个 pattern vs 1000 个 pattern QPS 相同（~1.6M） |
| 线程安全 | 外部 shared_mutex 保护（读写分离） |
| 内存 | `unordered_map<string, unique_ptr<Node>>` 树结构，移除后自动修剪空节点 |
| 查找零分配 | 透明哈希（`SVHash`/`SVEqual`），`find(string_view)` 不构造临时 `std::string` |
| `empty()` | O(1) `entry_count_` 计数器，非递归遍历 |

---

### 4.8 TopicSlot — COW 订阅管理

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

### 4.9 MessageBus — 核心总线

**文件**: `include/msgbus/message_bus.h`

```cpp
class MessageBus {
public:
    /// @param queue_capacity  主队列容量（默认 kDefaultQueueCapacity = 65536）
    /// @param num_dispatchers dispatcher 线程数（默认 1，0 = 自动 = hardware_concurrency）
    explicit MessageBus(size_t queue_capacity = kDefaultQueueCapacity, unsigned num_dispatchers = 1);

    void start();   // 启动 dispatcher 线程
    void stop();    // 停止并排空队列

    template <typename T>
    bool publish(std::string_view topic, T msg);          // 发布（对象池 + 无锁入队）

    template <typename T, typename Handler>
    SubscriptionId subscribe(std::string_view topic, Handler&& handler); // 支持通配符

    void unsubscribe(SubscriptionId id);

    template <typename T>
    AsyncWaitAwaitable<T> async_wait(std::string_view topic); // 协程等待

    unsigned dispatcher_count() const;  // 返回 dispatcher 线程数
};
```

**Topic 类型安全**：

* 每个 topic 绑定首次 subscribe/publish 的类型 `T`
* 类型不匹配时抛出 `std::runtime_error`
* 使用 `std::shared_mutex` 实现读写分离：getOrCreateSlot 读多写少

**消息分发（dispatchMessage）**：

```
1. 精确匹配: slots_[topic_id] → TopicSlot<T>::dispatch()（整数哈希查找）
2. 通配符匹配: registry_.to_string(topic_id) → WildcardTrie::match()
   → O(topic depth) 遍历 Trie，返回匹配的 slot 列表
```

**单线程 Dispatcher（num_dispatchers == 1）**：

```
dispatchLoop():
  空闲计数 < kSpinThreshold(64)   → 自旋（最低延迟）
  空闲计数 < kYieldThreshold(256) → yield（让出 CPU）
  空闲计数 ≥ kYieldThreshold      → condition_variable wait（被 publish notify 唤醒）
  消息到达                        → 立即重置计数，回到自旋
```

**多线程 Dispatcher（num_dispatchers > 1）**：

```
架构: 1 Router 线程 + N Worker 线程

Router 线程 (routerLoop):
  从主队列 dequeue → hash(topic_id) % N → 入 worker_queues_[N]
  相同 topic 的消息总是路由到同一 worker（保序）

Worker 线程 (workerLoop):
  从 worker_queues_[id] dequeue → dispatchMessage()
  每个 worker 独立退避策略
```

**保序保证**：同一 topic 的消息通过 hash(topic_id) 分片始终路由到同一 worker，保证该 topic 内的投递顺序。

**优雅关闭**（安全时序）：

```
stop():
  1. running_ = false
  2. join Router 线程 → Router 排空主队列到各 worker 队列
  3. router_drained_ = true（原子标志，release 语义）
  4. join Worker 线程 → Worker 看到 router_drained_ 后做最终 drain
```

关键：**Router 先退出，Worker 后退出**，避免 Router drain 阶段推入的消息无人消费。
Worker 在 `running_=false` 后进入等待循环，直到 `router_drained_=true` 才做最终排空退出。

---

### 4.10 AsyncWaitAwaitable — 协程支持

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
  → 消息到来: dispatcher 调用 handler
    → atomic CAS fired(false→true)，仅首次成功时:
      → 存结果 → resume handle
    → 后续触发直接跳过（防多 dispatcher + 通配符双发 race）
  → await_resume: unsubscribe, 返回结果
```

**安全保证**：
* `SharedState::fired`（`atomic<bool>`）CAS 保证 handler 最多触发一次
* 防止多 dispatcher 模式下通配符匹配多 topic 导致 `handle.resume()` 被多线程同时调用
* GCC 下需要 `template` 关键字处理依赖名: `s->bus.template subscribe<T>()`

---

## 五、关键设计决策

### 5.1 无锁层次

| 操作 | 锁级别 | 说明 |
|------|--------|------|
| `publish` | shared_lock | TopicRegistry resolve + 对象池 acquire + 原子 CAS 入队 |
| `subscribe` (精确) | mutex (COW) | 低频操作，复制整个列表 |
| `subscribe` (通配符) | shared_mutex 写锁 | 追加 WildcardEntry |
| `unsubscribe` | mutex / shared_mutex | 同上 |
| `dispatch` (精确) | **读无锁** | TopicId 整数哈希查找，仅 shared_ptr 拷贝加锁，遍历无锁 |
| `dispatch` (通配符) | shared_mutex 读锁 | Trie 遍历 O(depth)，不随模式数增长 |
| Topic 查找 | shared_mutex 读锁 | TopicId 整数键哈希表查找，读多写少 |
| TopicRegistry resolve | shared_lock / unique_lock | 已注册只需 shared_lock，首次注册 unique_lock |
| Router 路由 | **完全无锁** | hash(TopicId) + CAS 入 worker 队列 |

### 5.2 内存管理

* **消息生命周期**：`MessagePtr`（侵入式引用计数）从 publish 到最后一个 handler 完成后 → recycler 回收到对象池
* **对象池回收**：`TypedMessagePool<T>` 静态单例，容量 `kDefaultPoolCapacity`(8192)，满则 delete
* **订阅者列表**：`shared_ptr<vector<Subscriber>>` COW，读者持有快照不受写者影响
* **协程状态**：`shared_ptr<SharedState>` 确保 handler 回调和 awaitable 共享安全

### 5.3 类型系统

* `template<T>` 编译期类型安全
* `IMessage` + `typeid` 运行时校验
* 同一 topic 只允许一种类型，防止 `static_cast` 错误
* 通配符订阅同样做类型校验，跳过类型不匹配的消息
* 通配符格式校验：`#` 必须为最后一段，否则抛出 `std::runtime_error`

---

## 六、性能设计

### 热路径分析

| 路径 | 复杂度 | 锁 | 优化 |
|------|--------|----|------|
| publish → registry resolve → pool acquire → enqueue | O(1) | shared_lock + 无锁 | TopicId 整数赋值 + 对象池避免堆分配 |
| dequeue → dispatch | O(subscribers + wildcards) | 读无锁/读锁 | TopicId 整数哈希 + COW 快照 |
| subscribe | O(n) copy | mutex | 低频，COW |

### 缓存友好

* 队列 enqueue/dequeue 计数器分别 64 字节对齐，避免 false sharing
* 环形队列连续内存，顺序访问
* 对象池复用减少 cache miss（热对象保留在缓存中）

### Zero-copy 优化

* 侵入式引用计数：消除 `shared_ptr` 独立 control block 的堆分配
* 对象池 + `reset()`：消息对象复用，避免构造/析构开销
* recycler 函数指针：零虚函数开销的回收路径
* TopicId 整数路由：dispatch/route 路径上用 `uint32_t` 替代 `std::string` 哈希/比较
* 通配符 Trie 索引：O(topic depth) 匹配替代 O(N) 线性扫描
* 条件变量退避：替代 sleep，空闲时被 publish notify 快速唤醒

---

## 七、未来演进方向

| 方向 | 优先级 | 状态 | 说明 |
|------|--------|------|------|
| ~~多线程 dispatcher~~ | ~~P2~~ | ✅ 已实现 | 按 topic hash 分片到线程池 |
| ~~Zero-copy 优化~~ | ~~P3~~ | ✅ 已实现 | 对象池 + 侵入式引用计数 |
| ~~Topic 通配符~~ | ~~P3~~ | ✅ 已实现 | MQTT 风格 `*` / `#` |
| ~~TopicId 整数路由~~ | ~~P2~~ | ✅ 已实现 | TopicRegistry + uint32_t 替代 string，多 Topic +86% |
| ~~通配符 Trie 索引~~ | ~~P2~~ | ✅ 已实现 | O(depth) 匹配，不随 pattern 数增长 |
| ~~条件变量退避~~ | ~~P2~~ | ✅ 已实现 | 替代 sleep，p50 延迟 ~7x |
| Backpressure | P2 | 待实现 | publish 返回 false 时限流策略 |
| Batch 消费 | P2 | 待实现 | 一次 dequeue 多条，减少调度开销 |
| 优先级队列 | P3 | 待实现 | 消息优先级 |
| 延迟消息 | P3 | 待实现 | 定时投递 |
| 持久化 | P3 | 待实现 | 扩展为轻量 MQ |
| 跨进程 IPC | P3 | 待实现 | 共享内存 + 信号量 |

---

## 八、实现进度

### 基础实现
- [x] 实现 Lock-Free Queue（Vyukov bounded MPMC）
- [x] 实现 MessageBus 基础结构
- [x] 实现 publish（无锁路径）
- [x] 实现 subscribe（copy-on-write）
- [x] 实现 unsubscribe

### Zero-copy 优化
- [x] 实现 ObjectPool（无锁 freelist）
- [x] 实现侵入式 MessagePtr（替代 shared_ptr）
- [x] 实现 TypedMessagePool 每类型静态池
- [x] publish 路径对象复用（acquire → reset → enqueue）
- [x] 引用计数归零自动回收到池

### TopicId 整数路由
- [x] 实现 TopicRegistry（线程安全双向映射，shared_mutex 读写分离）
- [x] IMessage::topic_id() 替代 topic()，TypedMessage 携带 uint32_t
- [x] slots_ / topic_types_ 以 TopicId 为键（整数哈希）
- [x] routeToWorker 使用 hash(TopicId) 路由
- [x] 通配符 unsubscribe 无需反查字符串（SubInfo 标志区分）

### 通配符 Trie 索引
- [x] 实现 WildcardTrie（按 topic 层级建立 Trie 树）
- [x] 匹配复杂度 O(depth)，与模式数量无关
- [x] 替代 wildcard_entries_ 线性扫描
- [x] WildcardTrie 单元测试（12 个用例）
- [x] 透明哈希（`SVHash`/`SVEqual`）实现 `find(string_view)` 零分配
- [x] O(1) `entry_count_` 替代递归 `isEmpty()` 遍历
- [x] `remove()` 后自动修剪空 Trie 节点（防内存泄漏）
- [x] `insert()` 接受 `string_view pattern` 参数，Entry 不存储冗余 pattern 字符串

### 条件变量退避
- [x] dispatch/router/worker 线程用 condition_variable 替代 sleep
- [x] publish 时 notify_one 唤醒 dispatcher
- [x] routeToWorker 时 notify_one 唤醒对应 worker
- [x] stop() 时 notify_all 唤醒所有线程

### 多 Dispatcher
- [x] 实现 Router 线程（hash 分片路由）
- [x] 实现 Worker 线程池（独立退避）
- [x] 同 topic 保序（hash 到同一 worker）
- [x] 构造函数 num_dispatchers 参数（0=auto）

### 通配符订阅
- [x] 实现 topicMatches（MQTT 风格 `*` / `#`）
- [x] 实现 isWildcard 检测
- [x] subscribe 自动识别通配符 vs 精确匹配
- [x] dispatchMessage 先精确后通配符
- [x] unsubscribe 通配符清理
- [x] 通配符格式校验（`#` 必须为最后一段，否则抛异常）

### API 优化
- [x] `publish()`/`subscribe()`/`async_wait()` topic 参数从 `const std::string&` 改为 `std::string_view`

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
- [x] async_wait 协程防双发（atomic fired guard）
- [x] 多 dispatcher 安全关闭（router_drained_ 时序保证）
- [x] getOrCreateSlot 读锁下安全查找（消除 UB）
- [x] 命名常量替代魔数
- [ ] 日志系统接入
- [ ] metrics（队列长度、延迟）

### 工程化
- [x] 单元测试（80 个 GTest 用例）
- [x] 代码覆盖率统计（OpenCppCoverage / lcov，98.2%）
- [x] 性能压测（8 个基准场景）
- [x] GitHub Actions CI（Windows/Linux/macOS，UT 门禁）
- [x] Release 工作流（tag 预发布 + 压测 + 源码包）
- [x] 文档完善（设计文档 + 使用文档 + 性能文档）
- [x] 示例代码（基础 + 协程 + 通配符）

### 待实现
- [ ] 引入 MPMC 高性能队列（moodycamel 对比）
- [ ] 支持优先级队列
- [ ] 支持延迟消息
- [ ] 支持跨进程通信（IPC）
- [ ] Backpressure 限流策略
- [ ] Batch 消费优化
