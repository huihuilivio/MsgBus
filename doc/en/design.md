# MessageBus Design Document

[中文](../zh/design.md)

## 1. Project Goals

Implement a **single-process, high-performance, lock-free (or minimal-lock), coroutine-capable message bus system** for module decoupling and event-driven architectures.

### Core Capabilities

* High-performance intra-process communication
* Topic (string) routing + MQTT-style wildcards
* Asynchronous processing (non-blocking publish)
* Essentially lock-free (lock-free / wait-free)
* C++20 coroutine support
* Subscribe / unsubscribe support
* Custom message type support
* Zero-copy object pool + intrusive reference counting
* Multi-dispatcher thread pool (topic hash sharding)

---

## 2. Overall Architecture

```
                ┌───────────────────────────────────────────────────────────┐
                │                      MessageBus                          │
                │                                                          │
  Producer ─────┤  publish<T>(topic, msg)                                  │
  Producer ─────┤       │  ① Pool acquire / new TypedMessage               │
  Producer ─────┤       │  ② Set recycler callback                         │
                │       ▼                                                  │
                │  ┌─────────────────────────┐                             │
                │  │  Lock-Free Queue         │  MPMC Ring Buffer          │
                │  │  (Vyukov algorithm)      │  Capacity auto-aligned 2^N │
                │  └────────┬────────────────┘                             │
                │           │                                              │
                │     ┌─────┴─────────────────────────────────┐            │
                │     │ num_dispatchers == 1?                  │            │
                │     │                                        │            │
                │  ┌──┴──────────────┐    ┌───────────────────┴──────────┐ │
                │  │ Single Dispatch  │    │ Multi-Dispatcher (Pool)      │ │
                │  │ dispatchLoop()  │    │                              │ │
                │  │ 3-tier backoff  │    │  Router thread               │ │
                │  │ spin→yield      │    │  hash(topic) % N → queues   │ │
                │  │ →sleep          │    │                              │ │
                │  └──────┬──────────┘    │  Worker[0] Worker[1] ... [N] │ │
                │         │               │  Each dispatches independently│ │
                │         │               └──────────┬───────────────────┘ │
                │         └────────┬─────────────────┘                     │
                │                  ▼  Route by topic                       │
                │  ┌────────────────────────────────────────┐              │
                │  │  ① Exact match: TopicSlot<T> (COW list) │              │
                │  │  ② Wildcard match: WildcardTrie index   │              │
                │  │     '*' one level / '#' multi-level     │              │
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
                │                  ▼  When ref count reaches zero           │
                │  ┌────────────────────────────────────────┐              │
                │  │  ObjectPool<TypedMessage<T>> recycle    │              │
                │  └────────────────────────────────────────┘              │
                └───────────────────────────────────────────────────────────┘
```

---

## 3. Project Structure

```
MsgBus/
├── CMakeLists.txt                      # Root build file
├── doc/
│   ├── en/                             # English documentation
│   │   ├── README.md
│   │   ├── design.md                   # Design document (this file)
│   │   └── BENCHMARK.md
│   └── zh/                             # Chinese documentation
│       ├── README.md
│       ├── design.md
│       └── BENCHMARK.md
├── .gitignore
├── include/
│   └── msgbus/
│       ├── lock_free_queue.h           # MPMC lock-free ring buffer
│       ├── message.h                   # IMessage intrusive ref count + TypedMessage<T> + MessagePtr
│       ├── object_pool.h              # Lock-free object pool (freelist recycling)
│       ├── subscriber.h               # Subscriber<T> definition
│       ├── topic_matcher.h            # MQTT-style topic wildcard matching
│       ├── topic_registry.h           # TopicId ↔ string thread-safe registry
│       ├── topic_slot.h               # TopicSlot<T> COW subscription management
│       ├── wildcard_trie.h            # Wildcard trie index (O(depth) matching)
│       └── message_bus.h              # MessageBus core + pool + multi-dispatcher + wildcards + coroutines + TopicRegistry
├── examples/
│   ├── CMakeLists.txt
│   ├── basic_example.cpp              # Basic pub/sub example
│   ├── coroutine_example.cpp          # C++20 coroutine example
│   └── wildcard_example.cpp           # Wildcard subscription example
├── tests/
│   ├── CMakeLists.txt
│   ├── test_message_bus.cpp            # Unit tests (80 GTest cases)
│   └── bench_message_bus.cpp           # Benchmarks (8 scenarios)
└── .github/
    └── workflows/
        ├── ci.yml                      # CI: UT on push/PR (Windows/Linux/macOS)
        └── release.yml                 # Release: tag pre-release + benchmarks + source archive
```

---

## 4. Core Module Design

### 4.1 LockFreeQueue — Lock-Free Ring Buffer

**File**: `include/msgbus/lock_free_queue.h`

Based on Dmitry Vyukov's bounded MPMC queue algorithm.

```cpp
template <typename T>
class LockFreeQueue {
public:
    explicit LockFreeQueue(size_t capacity);
    bool try_enqueue(T value);   // Enqueue; returns false if full
    bool try_dequeue(T& value);  // Dequeue; returns false if empty
};
```

**Key Design**:

| Feature | Implementation |
|---|---|
| Algorithm | Vyukov bounded MPMC, each Cell has a sequence atomic counter |
| Capacity | Auto-aligned to 2^N, uses bitmask modulo (avoids % overhead) |
| Memory Order | enqueue/dequeue positions use `relaxed`, Cell sequences use `acquire/release` |
| Cache Line | enqueue_pos\_ and dequeue_pos\_ each `alignas(64)` to avoid false sharing |
| Thread Safety | Fully lock-free, supports any number of concurrent producers/consumers |

**Reuse**: Main queue, multi-dispatcher worker queues, and object pool freelist all reuse this queue.

---

### 4.2 ObjectPool — Lock-Free Object Pool

**File**: `include/msgbus/object_pool.h`

Lock-free freelist-based object pool built on LockFreeQueue for message object recycling.

```cpp
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(size_t capacity);   // Freelist max capacity
    T* acquire();                           // Get recycled object; nullptr if empty
    void release(T* p);                     // Return object; delete if pool is full
};
```

**Design Points**:
* No pre-allocation; caches objects after first use
* Both `acquire()` and `release()` are lock-free (backed by `LockFreeQueue<T*>`)
* `release()` deletes directly when pool is full to avoid unbounded growth
* Destructor drains freelist and deletes all cached objects

---

### 4.3 Message — Intrusive Reference-Counted Messages

**File**: `include/msgbus/message.h`

```cpp
/// Compact topic identifier — replaces std::string on the hot path.
using TopicId = uint32_t;
inline constexpr TopicId kInvalidTopicId = 0;

struct IMessage {
    std::atomic<int> ref_count_{0};         // Intrusive reference count
    void (*recycler_)(IMessage*) = nullptr; // Recycle function pointer

    virtual TopicId topic_id();
    virtual const std::type_info& type();
    void add_ref() noexcept;
    bool release_ref() noexcept;            // Returns true when count reaches zero
};

template <typename T>
struct TypedMessage : IMessage {
    TopicId topic_id_;
    T data_;
    void reset(TopicId topic_id, T data); // Reset for pool reuse (integer assign instead of string copy)
};
```

**MessagePtr — Intrusive Smart Pointer**:

```cpp
class MessagePtr {
public:
    static MessagePtr adopt(IMessage* p) noexcept;  // Takes ownership and calls add_ref
    // Copy/move semantics; destructor calls release_ref
    // When ref count reaches zero: has recycler_ → call recycle; otherwise delete
};
```

**Design Points**:

* **Replaces `shared_ptr`**: Eliminates separate control block heap allocation; ref count embedded in IMessage
* **Recycler function pointer**: Set during publish; called when ref count reaches zero to return object to pool instead of delete
* **`reset()` method**: Resets topic_id/data/ref_count/recycler for pool reuse, integer assign instead of string copy
* Performance gain: Minimum latency from ~36μs down to ~0.6μs

---

### 4.4 TypedMessagePool — Per-Type Static Object Pool

**File**: `include/msgbus/message_bus.h` (embedded)

```cpp
// Tuning constants (named, not magic numbers)
inline constexpr size_t kDefaultQueueCapacity  = 65536;
inline constexpr size_t kDefaultPoolCapacity    = 8192;
inline constexpr unsigned kSpinThreshold        = 64;
inline constexpr unsigned kYieldThreshold       = 256;

template <typename T>
struct TypedMessagePool {
    static ObjectPool<TypedMessage<T>>& instance();   // Static singleton, capacity kDefaultPoolCapacity
    static void recycle(IMessage* msg);                // Recycle callback
};
```

**Publish Path**:

```
publish<T>(topic, msg)
  → registry_.resolve(topic) → TopicId  // string→integer (shared_lock fast path)
  → pool.acquire()
  → hit:  reset(topic_id, msg)    // Reuse, integer assign, no heap allocation
  → miss: new TypedMessage<T>     // First-time creation
  → raw->recycler_ = &recycle     // Set recycle callback
  → queue_.try_enqueue(MessagePtr::adopt(raw))
```

**Recycle Path**:

```
MessagePtr ref count reaches zero
  → recycler_(ptr)
  → TypedMessagePool<T>::recycle(msg)
  → pool.release(static_cast<TypedMessage<T>*>(msg))
  → Pool not full: cache for reuse / Pool full: delete
```

---

### 4.5 TopicRegistry — Topic String ↔ ID Registry

**File**: `include/msgbus/topic_registry.h`

Thread-safe bidirectional mapping between topic strings and integer IDs. All hot paths (dispatch, route) use `TopicId` (`uint32_t`) instead of `std::string` for hashing and comparison.

```cpp
class TopicRegistry {
public:
    TopicId resolve(std::string_view topic);      // Register or look up topic → stable ID
    std::string_view to_string(TopicId id) const; // Reverse lookup ID → string (e.g. for wildcard matching)
};
```

**Key Design**:

| Feature | Implementation |
|---|---|
| Read path | `shared_lock` + transparent hash lookup (`string_view` into `string` keys, zero allocation) |
| Write path | `unique_lock` + double-check, only triggered on first registration |
| ID stability | Monotonically incrementing, `kInvalidTopicId = 0`, valid IDs start from 1 |
| String lifetime | `id_to_sv_` stores `string_view` pointing into `unordered_map` keys (iterator stability guarantee) |

**Benefits**:
* dispatch path `slots_` and `topic_types_` changed from `unordered_map<string, ...>` to `unordered_map<uint32_t, ...>`
* Routing via `hash<TopicId>` instead of `hash<string>` in `routeToWorker`
* `TypedMessage<T>` carries only 4-byte `TopicId` instead of `std::string`
* Multi-topic QPS improved ~26%, p50 latency reduced ~6x

---

### 4.6 TopicMatcher — MQTT-Style Wildcards

**File**: `include/msgbus/topic_matcher.h`

```cpp
bool topicMatches(std::string_view pattern, std::string_view topic);
bool isWildcard(std::string_view pattern);
```

**Wildcard Rules** (MQTT-style):

| Wildcard | Semantics | Example |
|---|---|---|
| `*` | Matches exactly one level | `sensor/*/temp` matches `sensor/1/temp`, not `sensor/1/2/temp` |
| `#` | Matches zero or more trailing levels (must be last segment) | `sensor/#` matches `sensor`, `sensor/temp`, `sensor/1/temp` |

**Edge Cases**:
* `sensor/#` matches `sensor` (zero levels — `#` matches empty)
* `a/*/c/#` matches `a/x/c` (`#` matches zero levels)
* Pure exact patterns (no `*` or `#`) take the fast path (direct hash lookup)

---

### 4.7 WildcardTrie — Wildcard Trie Index

**File**: `include/msgbus/wildcard_trie.h`

Builds a trie from wildcard subscription patterns indexed by topic levels. On dispatch, the trie is walked with the concrete topic's levels, achieving **O(topic depth)** complexity instead of O(N).

```cpp
class WildcardTrie {
public:
    void insert(std::string_view pattern,  // Insert wildcard pattern
               const Entry& entry);
    bool remove(SubscriptionId id);        // Remove by ID (auto-prunes empty nodes)
    void match(std::string_view topic,     // Find matching slots
               const std::type_info& type,
               std::vector<ITopicSlot*>& out) const;
    bool empty() const;                    // O(1) entry counter
};
```

**Matching Algorithm**:

```
matchNode(node, levels, depth):
  1. Check node's '#' child → matches all remaining levels (terminal)
  2. depth == levels.size() → check entries at current node
  3. Check exact-match child levels[depth] → recurse depth+1
  4. Check '*' child → recurse depth+1 (skip one level)
```

**Key Design**:

| Feature | Implementation |
|---|---|
| Complexity | O(topic depth), independent of pattern count |
| Scalability | 100 patterns vs 1000 patterns: same QPS (~1.6M) |
| Thread safety | External shared_mutex protection (read-write separation) |
| Memory | `unordered_map<string, unique_ptr<Node>>` tree structure, auto-prunes empty nodes on remove |
| Zero-alloc lookup | Transparent hash (`SVHash`/`SVEqual`), `find(string_view)` avoids temporary `std::string` |
| `empty()` | O(1) `entry_count_` counter, not recursive traversal |

---

### 4.8 TopicSlot — COW Subscription Management

**File**: `include/msgbus/topic_slot.h`

```cpp
struct ITopicSlot {
    virtual void dispatch(const MessagePtr& msg) = 0;
    virtual bool removeSubscriber(SubscriptionId id) = 0;
};

template <typename T>
class TopicSlot : public ITopicSlot { ... };
```

**Copy-on-Write Strategy**:

```
subscribe/unsubscribe:
  1. lock(mutex)
  2. Copy current subscriber list → new shared_ptr<vector>
  3. Modify new list
  4. Atomically replace pointer
  5. unlock

dispatch:
  1. lock(mutex) → copy shared_ptr → unlock
  2. Iterate snapshot, call handlers (lock-free)
```

* **Read path (dispatch)**: Holds lock only to copy one shared_ptr; iteration is fully lock-free
* **Write path (subscribe/unsubscribe)**: Low-frequency, mutex-protected COW
* **Exception isolation**: One handler throwing does not affect other subscribers

---

### 4.9 MessageBus — Core Bus

**File**: `include/msgbus/message_bus.h`

```cpp
class MessageBus {
public:
    /// @param queue_capacity  Main queue capacity (default kDefaultQueueCapacity = 65536)
    /// @param num_dispatchers Dispatcher thread count (default 1, 0 = auto = hardware_concurrency)
    explicit MessageBus(size_t queue_capacity = kDefaultQueueCapacity, unsigned num_dispatchers = 1);

    void start();   // Start dispatcher thread(s)
    void stop();    // Stop and drain queue

    template <typename T>
    bool publish(std::string_view topic, T msg);          // Publish (pool + lock-free enqueue)

    template <typename T, typename Handler>
    SubscriptionId subscribe(std::string_view topic, Handler&& handler); // Supports wildcards

    void unsubscribe(SubscriptionId id);

    template <typename T>
    AsyncWaitAwaitable<T> async_wait(std::string_view topic); // Coroutine await

    unsigned dispatcher_count() const;  // Returns dispatcher thread count
};
```

**Topic Type Safety**:

* Each topic binds to the type `T` of the first subscribe/publish call
* Type mismatch throws `std::runtime_error`
* Uses `std::shared_mutex` for read-write separation: getOrCreateSlot is read-heavy

**Message Dispatch (dispatchMessage)**:

```
1. Exact match: slots_[topic_id] → TopicSlot<T>::dispatch() (integer hash lookup)
2. Wildcard match: registry_.to_string(topic_id) → WildcardTrie::match()
   → O(topic depth) trie traversal, returns matching slot list
```

**Single-Thread Dispatcher (num_dispatchers == 1)**:

```
dispatchLoop():
  idle count < kSpinThreshold(64)   → spin (lowest latency)
  idle count < kYieldThreshold(256) → yield (give up CPU)
  idle count ≥ kYieldThreshold      → condition_variable wait (woken by publish notify)
  message arrives                   → immediately reset count, back to spin
```

**Multi-Thread Dispatcher (num_dispatchers > 1)**:

```
Architecture: 1 Router thread + N Worker threads

Router thread (routerLoop):
  Dequeue from main queue → hash(topic_id) % N → enqueue to worker_queues_[N]
  Same topic always routes to same worker (ordering guarantee)

Worker thread (workerLoop):
  Dequeue from worker_queues_[id] → dispatchMessage()
  Each worker has independent backoff strategy
```

**Ordering Guarantee**: Messages on the same topic are hash-sharded via hash(topic_id) to the same worker, guaranteeing in-order delivery within that topic.

**Graceful Shutdown** (safe sequence):

```
stop():
  1. running_ = false
  2. Join Router thread → Router drains main queue into worker queues
  3. router_drained_ = true (atomic flag, release semantics)
  4. Join Worker threads → Workers see router_drained_ and do final drain
```

Key: **Router exits first, Workers exit last**, ensuring no messages pushed during Router drain phase go unconsumed.
Workers enter a wait loop after `running_=false`, only doing final drain and exit after `router_drained_=true`.

---

### 4.10 AsyncWaitAwaitable — Coroutine Support

```cpp
template <typename T>
class AsyncWaitAwaitable {
    // await_suspend: register one-shot subscription, resume coroutine on message
    // await_resume: return message value, auto-unsubscribe
};
```

**Uses SharedState** for shared ownership, avoiding awaitable lifetime issues (MSVC coroutine frame management).

```
co_await bus.async_wait<T>(topic)
  → await_suspend: subscribe, save coroutine_handle
  → message arrives: dispatcher calls handler
    → atomic CAS fired(false→true), only on first success:
      → store result → resume handle
    → subsequent triggers skip (prevents multi-dispatcher + wildcard double-resume race)
  → await_resume: unsubscribe, return result
```

**Safety Guarantees**:
* `SharedState::fired` (`atomic<bool>`) CAS ensures handler fires at most once
* Prevents `handle.resume()` being called concurrently from multiple threads in multi-dispatcher + wildcard scenarios
* GCC requires `template` keyword for dependent names: `s->bus.template subscribe<T>()`

---

## 5. Key Design Decisions

### 5.1 Lock Hierarchy

| Operation | Lock Level | Notes |
|---|---|---|
| `publish` | shared_lock | TopicRegistry resolve + pool acquire + atomic CAS enqueue |
| `subscribe` (exact) | mutex (COW) | Low-frequency, copies entire list |
| `subscribe` (wildcard) | shared_mutex write lock | Append WildcardEntry |
| `unsubscribe` | mutex / shared_mutex | Same as above |
| `dispatch` (exact) | **Read lock-free** | TopicId integer hash lookup; only shared_ptr copy under lock; iteration lock-free |
| `dispatch` (wildcard) | shared_mutex read lock | Trie traversal O(depth), independent of pattern count |
| Topic lookup | shared_mutex read lock | TopicId integer-key hash table lookup, read-heavy |
| TopicRegistry resolve | shared_lock / unique_lock | Already registered: shared_lock only; first registration: unique_lock |
| Router routing | **Fully lock-free** | hash(TopicId) + CAS into worker queue |

### 5.2 Memory Management

* **Message lifetime**: `MessagePtr` (intrusive ref count) from publish to last handler completion → recycler returns to pool
* **Object pool recycling**: `TypedMessagePool<T>` static singleton, capacity `kDefaultPoolCapacity`(8192), deletes when full
* **Subscriber list**: `shared_ptr<vector<Subscriber>>` COW, readers hold snapshots unaffected by writers
* **Coroutine state**: `shared_ptr<SharedState>` ensures handler callback and awaitable share safely

### 5.3 Type System

* `template<T>` compile-time type safety
* `IMessage` + `typeid` runtime verification
* One type per topic prevents `static_cast` errors
* Wildcard subscriptions also perform type checks, skipping type-mismatched messages
* Wildcard format validation: `#` must be the last segment, otherwise throws `std::runtime_error`

---

## 6. Performance Design

### Hot Path Analysis

| Path | Complexity | Lock | Optimization |
|---|---|---|---|
| publish → registry resolve → pool acquire → enqueue | O(1) | shared_lock + lock-free | TopicId integer assign + object pool avoids heap allocation |
| dequeue → dispatch | O(subscribers + wildcards) | Read lock-free / read lock | TopicId integer hash + COW snapshot |
| subscribe | O(n) copy | mutex | Low-frequency, COW |

### Cache Friendliness

* Queue enqueue/dequeue counters 64-byte aligned to avoid false sharing
* Ring buffer contiguous memory, sequential access
* Object pool reuse reduces cache misses (hot objects stay in cache)

### Zero-Copy Optimization

* Intrusive ref counting: eliminates `shared_ptr` separate control block heap allocation
* Object pool + `reset()`: message object reuse avoids construct/destruct overhead
* Recycler function pointer: zero virtual function overhead recycle path
* TopicId integer routing: dispatch/route paths use `uint32_t` instead of `std::string` hash/compare
* Wildcard trie index: O(topic depth) matching replaces O(N) linear scan
* Condition variable backoff: replaces sleep, idle threads woken quickly by publish notify

---

## 7. Future Directions

| Direction | Priority | Status | Notes |
|---|---|---|---|
| ~~Multi-thread dispatcher~~ | ~~P2~~ | ✅ Done | Topic hash sharding to thread pool |
| ~~Zero-copy optimization~~ | ~~P3~~ | ✅ Done | Object pool + intrusive ref count |
| ~~Topic wildcards~~ | ~~P3~~ | ✅ Done | MQTT-style `*` / `#` |
| ~~TopicId integer routing~~ | ~~P2~~ | ✅ Done | TopicRegistry + uint32_t replaces string, multi-topic +86% |
| ~~Wildcard trie index~~ | ~~P2~~ | ✅ Done | O(depth) matching, independent of pattern count |
| ~~Condition variable backoff~~ | ~~P2~~ | ✅ Done | Replaces sleep, p50 latency ~7x |
| Backpressure | P2 | Planned | Rate limiting strategy when publish returns false |
| Batch consumption | P2 | Planned | Dequeue multiple at once, reduce scheduling overhead |
| Priority queue | P3 | Planned | Message priorities |
| Delayed messages | P3 | Planned | Timed delivery |
| Persistence | P3 | Planned | Extend to lightweight MQ |
| Cross-process IPC | P3 | Planned | Shared memory + semaphores |

---

## 8. Implementation Progress

### Core Implementation
- [x] Lock-Free Queue (Vyukov bounded MPMC)
- [x] MessageBus basic structure
- [x] publish (lock-free path)
- [x] subscribe (copy-on-write)
- [x] unsubscribe

### Zero-Copy Optimization
- [x] ObjectPool (lock-free freelist)
- [x] Intrusive MessagePtr (replaces shared_ptr)
- [x] TypedMessagePool per-type static pool
- [x] Publish path object reuse (acquire → reset → enqueue)
- [x] Automatic pool recycling on ref count zero

### TopicId Integer Routing
- [x] TopicRegistry (thread-safe bidirectional mapping, shared_mutex read-write separation)
- [x] IMessage::topic_id() replaces topic(), TypedMessage carries uint32_t
- [x] slots_ / topic_types_ keyed by TopicId (integer hash)
- [x] routeToWorker uses hash(TopicId) routing
- [x] Wildcard unsubscribe without string reverse lookup (SubInfo flag distinction)

### Wildcard Trie Index
- [x] WildcardTrie (trie built from topic levels)
- [x] Matching complexity O(depth), independent of pattern count
- [x] Replaces wildcard_entries_ linear scan
- [x] WildcardTrie unit tests (12 cases)
- [x] Transparent hash (`SVHash`/`SVEqual`) for zero-allocation `find(string_view)`
- [x] O(1) `entry_count_` replaces recursive `isEmpty()` traversal
- [x] `remove()` auto-prunes empty trie nodes (prevents memory leak)
- [x] `insert()` accepts `string_view pattern` parameter, Entry no longer stores redundant pattern string

### Condition Variable Backoff
- [x] dispatch/router/worker threads use condition_variable instead of sleep
- [x] publish notifies dispatcher via notify_one
- [x] routeToWorker notifies corresponding worker
- [x] stop() wakes all threads via notify_all

### Multi-Dispatcher
- [x] Router thread (hash sharding)
- [x] Worker thread pool (independent backoff)
- [x] Same-topic ordering (hash to same worker)
- [x] Constructor num_dispatchers parameter (0=auto)

### Wildcard Subscriptions
- [x] topicMatches (MQTT-style `*` / `#`)
- [x] isWildcard detection
- [x] subscribe auto-detects wildcard vs. exact
- [x] dispatchMessage: exact first, then wildcard
- [x] unsubscribe wildcard cleanup
- [x] Wildcard format validation (`#` must be last segment, throws on violation)

### API Optimization
- [x] `publish()`/`subscribe()`/`async_wait()` topic parameter changed from `const std::string&` to `std::string_view`

### Dispatch Logic
- [x] Dispatcher thread (3-tier backoff)
- [x] Topic routing (shared_mutex read-write separation)
- [x] Handler invocation (exception isolation)

### Coroutine Support
- [x] AsyncWaitAwaitable
- [x] async_wait\<T\>
- [x] Coroutine resume (SharedState safe management)

### Reliability
- [x] Exception isolation (handler crash protection)
- [x] async_wait double-resume prevention (atomic fired guard)
- [x] Multi-dispatcher safe shutdown (router_drained_ sequence guarantee)
- [x] getOrCreateSlot safe lookup under read lock (UB eliminated)
- [x] Named constants replace magic numbers
- [ ] Logging system integration
- [ ] Metrics (queue depth, latency)

### Engineering
- [x] Unit tests (80 GTest cases)
- [x] Code coverage (OpenCppCoverage / lcov, 98.2%)
- [x] Benchmarks (8 scenarios)
- [x] GitHub Actions CI (Windows/Linux/macOS, UT gate)
- [x] Release workflow (tag pre-release + benchmarks + source archive)
- [x] Documentation (design doc + usage doc + benchmark doc)
- [x] Examples (basic + coroutine + wildcard)

### Planned
- [ ] Evaluate high-performance MPMC queue alternatives (moodycamel comparison)
- [ ] Priority queue support
- [ ] Delayed message support
- [ ] Cross-process communication (IPC)
- [ ] Backpressure rate limiting
- [ ] Batch consumption optimization
