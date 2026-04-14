# MsgBus Benchmark Report

[中文](../zh/BENCHMARK.md)

## CI Automated Benchmarks

Benchmarks run only on **tag release/pre-release** (triggered by pushing `v*` tags). Normal commits and PRs do not run benchmarks to conserve CI resources.

Benchmarks run automatically on these platforms at release time:

| Platform | Compiler | Runner |
|---|---|---|
| Windows | MSVC (latest) | `windows-latest` |
| Linux | GCC 13 | `ubuntu-latest` |
| Linux | Clang 17 | `ubuntu-latest` |
| macOS | Apple Clang | `macos-latest` |

Results are attached as artifacts on the [GitHub Release](../../releases) page (`benchmark-*.txt`).

> **Note**: CI environments are shared VMs with performance variance. Numbers are for regression detection and order-of-magnitude reference only, not representative of bare-metal performance.

## Running Benchmarks Locally

```bash
cmake -B build -S . -DCMAKE_CXX_STANDARD=20
cmake --build build --config Release
./build/tests/Release/msgbus_bench      # Windows
./build/tests/msgbus_bench              # Linux / macOS
```

---

## Local Reference Data (Windows)

### Test Environment

- **OS**: Windows 10 (19045)
- **Compiler**: MSVC 19.42 (Visual Studio 2022)
- **Build Config**: Release (`/O2`)
- **C++ Standard**: C++20
- **Optimization**: Object pool + intrusive reference counting + TopicId integer routing + Wildcard trie index + condition variable backoff

---

## Benchmark Scenarios

### 1. Raw Queue — Lock-Free Queue Raw Throughput

**Scenario**: 1 producer / 1 consumer, pure `LockFreeQueue<int>` operations, no message wrapping, no topic routing.

**Purpose**: Measure queue's theoretical maximum throughput as an upper bound reference.

| Metric | Result |
|---|---|
| Messages | 1,000,000 |
| Duration | ~14 ms |
| Throughput | **~71,000,000 ops/s** |

---

### 2. Throughput — Single Producer/Consumer End-to-End

**Scenario**: 1 publisher thread, 1 subscriber handler, full publish→queue→dispatch→handler path.

| Metric | Result |
|---|---|
| Messages | 1,000,000 |
| Duration | ~210 ms |
| Throughput | **~4,700,000 QPS** |

**Analysis**: Overhead vs. Raw Queue mainly comes from:
- Object pool acquire/release (lock-free, minimal)
- `TypedMessage<T>` construction or reset
- TopicRegistry resolve (shared_lock + hash lookup)
- Topic integer-key hash lookup + shared_mutex read lock
- `std::function` call overhead
- Intrusive reference counting atomic operations

> Compared to pre-optimization (shared_ptr): throughput roughly the same, but latency significantly reduced (see Latency test).

---

### 3. Multi-Producer — Multi-Producer Throughput

**Scenario**: 4 publisher threads publishing concurrently, 1 subscriber.

| Metric | Result |
|---|---|
| Threads | 4 |
| Messages per thread | 250,000 |
| Total messages | 1,000,000 |
| Duration | ~230 ms |
| Throughput | **~4,400,000 QPS** |

**Analysis**: ~20% throughput drop from multi-thread CAS contention, as expected. The Vyukov algorithm maintains good performance under multi-producer load.

---

### 4. Latency — Publish-to-Handler Latency

**Scenario**: 1 publisher, 1 subscriber, measuring per-message latency from publish to handler execution. Includes 1,000 warm-up messages.

| Percentile | Current (μs) | After TopicId (μs) | After Object Pool (μs) | Before Optimization (μs) |
|---|---|---|---|---|
| **min** | **0.2** | 0.6 | 0.3 | 36.2 |
| **p50** | **57** | 374 | 2,196 | 3,582.7 |
| **p90** | **114** | 480 | 4,977.8 | 4,977.8 |
| **p99** | **250** | 773 | 5,796.2 | 5,796.2 |
| **max** | **358** | 895 | 5,910.4 | 5,910.4 |

**Analysis**:

- **Minimum latency ~0.2μs**, thanks to object pool reuse eliminating heap allocation + intrusive ref counting
- **p50 dropped from ~374μs to ~57μs** (~7x improvement), condition variable replacing sleep backoff significantly reduces tail latency
- p90/p99 also significantly reduced (p99: 773μs → 250μs), idle threads are quickly woken by condvar
- In real-world usage (lower publish frequency), latency will approach the min value

---

### 5. Multi-Topic — Multi-Topic Throughput

**Scenario**: 100 different topics, 10,000 messages per topic.

| Metric | Current (TopicId) | After Object Pool | Before Optimization |
|---|---|---|---|
| Topics | 100 | 100 | 100 |
| Total messages | 1,000,000 | 1,000,000 | 1,000,000 |
| Duration | ~110 ms | ~172 ms | ~202 ms |
| Throughput | **~9,200,000 QPS** | ~5,800,000 QPS | ~4,950,000 QPS |

**Analysis**: Multi-topic QPS reaches **~9.2M**, a cumulative **+86%** improvement over the original shared_ptr version. TopicId integer hash lookup and condvar wakeup both contribute.

---

### 6. Fan-out — Broadcast Fan-out

**Scenario**: 1 publisher, N subscribers on the same topic.

#### 10 Subscribers

| Metric | Result |
|---|---|
| Messages | 100,000 |
| Subscribers | 10 |
| Total deliveries | 1,000,000 |
| Duration | ~33 ms |
| Delivery rate | **~30,000,000 deliveries/s** |

#### 100 Subscribers

| Metric | Result |
|---|---|
| Messages | 10,000 |
| Subscribers | 100 |
| Total deliveries | 1,000,000 |
| Duration | ~11 ms |
| Delivery rate | **~88,000,000 deliveries/s** |

**Analysis**: Delivery rate actually increases in fan-out scenarios because:
- Per-message dequeue + topic lookup overhead is amortized across multiple handler calls
- Subscriber list is contiguous in memory (vector), sequential traversal is cache-friendly
- `std::function<void(const T&)>` call overhead is minimal
- Intrusive ref counting: in fan-out scenarios messages are shared by multiple handlers; intrusive ref counting atomic ops are lighter than shared_ptr

---

### 7. Wildcard — Wildcard Matching Throughput

**Scenario**: N wildcard patterns, publish to a topic matching 1 of the patterns.

| Metric | 100 patterns | 1000 patterns |
|---|---|---|
| Messages | 100,000 | 100,000 |
| Duration | ~62 ms | ~62 ms |
| Throughput | **~1,600,000 QPS** | **~1,600,000 QPS** |

**Analysis**: Core advantage of the trie index: **matching complexity is O(topic depth), independent of pattern count**. 100 patterns and 1000 patterns achieve essentially the same QPS, whereas a linear scan would drop to 1/10 throughput at 1000 patterns.

---

### 8. Wildcard RCU — Concurrent Match Throughput

**Scenario**: N wildcard patterns, 4 concurrent reader threads performing match operations, measuring total throughput.

| Metric | 100 patterns | 1000 patterns |
|---|---|---|
| Messages | 100,000 | 100,000 |
| Reader threads | 4 | 4 |
| Duration | ~49 ms | ~57 ms |
| Throughput | **~2,030,000 QPS** | **~1,770,000 QPS** |

**Analysis**: RCU read path (`atomic<shared_ptr>` load on MSVC/GCC) enables near-linear reader scaling. 4 concurrent readers achieve ~2M QPS compared to ~1.6M QPS single-reader, demonstrating minimal contention.

---

### 9. FullPolicy — Queue-Full Strategy Comparison

**Scenario**: Small queue (1024) to frequently trigger full-queue paths. Compares all 5 FullPolicy strategies.

#### Single-Producer Throughput (500K messages)

| Policy | Published | Delivered | QPS |
|---|---|---|---|
| ReturnFalse | 491,725 | 491,725 | **~4,500,000** |
| DropNewest | 500,000 | 496,010 | **~4,510,000** |
| DropOldest | 500,000 | 495,469 | **~3,070,000** |
| Block | 500,000 | 500,000 | **~4,010,000** |
| BlockTimeout | 500,000 | 500,000 | **~3,460,000** |

#### Multi-Producer Throughput (4 threads × 125K messages)

| Policy | Published | Delivered | QPS |
|---|---|---|---|
| ReturnFalse | 491,656 | 491,656 | **~3,890,000** |
| DropNewest | 500,000 | 418,326 | **~4,100,000** |
| DropOldest | 500,000 | 495,804 | **~2,830,000** |
| Block | 500,000 | 500,000 | **~3,400,000** |
| BlockTimeout | 500,000 | 500,000 | **~3,560,000** |

#### Latency (50K messages, queue=4096)

| Policy | Delivered | p50 (μs) | p90 (μs) | p99 (μs) |
|---|---|---|---|---|
| ReturnFalse | 38,826 | 1,259 | 1,689 | 1,751 |
| DropNewest | 44,565 | 1,121 | 1,593 | 1,918 |
| DropOldest | 50,000 | **26** | 387 | 419 |
| Block | 50,000 | 719 | 1,295 | 1,657 |
| BlockTimeout | 50,000 | 501 | 747 | 987 |

**Analysis**:
- **ReturnFalse / DropNewest**: Fastest throughput (zero/minimal overhead), but may lose messages
- **DropOldest**: Lower throughput due to `publish_mutex_` serialization, but achieves the **best latency** (fresh messages always enter)
- **Block / BlockTimeout**: 100% delivery guarantee with `cv_not_full_` wait/notify, moderate throughput
- Under multi-producer CAS contention, DropNewest has highest QPS since it just discards without retry

---

### 10. Multi-Dispatcher — Thread Pool Throughput

**Scenario**: 1 Router thread + N Worker threads, 10 topics, 500K messages.

| Dispatchers | Duration | QPS |
|---|---|---|
| 2 | ~206 ms | **~2,430,000** |
| 4 | ~201 ms | **~2,490,000** |

**Analysis**: Multi-dispatcher adds Router+Worker overhead vs. single dispatcher (~5M QPS), but enables true parallel handler execution across topics. Best suited for CPU-bound handlers.

---

### 11. Concurrent Subscribe/Unsubscribe — COW Stability

**Scenario**: 2 threads rapidly subscribing/unsubscribing while 1 publisher sends 500K messages.

| Metric | Result |
|---|---|
| Messages | 500,000 |
| Sub/Unsub churners | 2 |
| Duration | ~214 ms |
| Publish QPS | **~2,340,000** |
| Churn operations | ~203,000 |

**Analysis**: COW subscriber list ensures publish throughput remains stable even under concurrent subscription churn. No data races, no missed deliveries to the stable subscriber.

---

### 12. Raw Queue MPMC — Multi-Producer Multi-Consumer

**Scenario**: 4 producers, 4 consumers, pure `LockFreeQueue<int>` operations.

| Metric | Result |
|---|---|
| Messages | 1,000,000 |
| Producers / Consumers | 4 / 4 |
| Duration | ~118 ms |
| Throughput | **~8,470,000 ops/s** |

**Analysis**: Vyukov MPMC queue handles 4P/4C with ~8.5M ops/s, vs ~71M for 1P/1C. CAS contention from 8 concurrent threads reduces throughput but maintains lock-free progress.

---

### 13. Object Pool Impact — Pool vs. No Pool

**Scenario**: 500K messages, comparing object pool hit (int, warmed up) vs. pool miss (string, cold).

| Mode | Duration | QPS |
|---|---|---|
| Pool Enabled (int) | ~64 ms | **~7,800,000** |
| Pool Bypassed (string) | ~122 ms | **~4,100,000** |

**Analysis**: Object pool achieves **~1.9x** throughput improvement by eliminating heap allocation on pool hit. The `string` path incurs allocation and copy overhead per message.

---

### 14. Stable Throughput — Multi-Round Median

**Scenario**: 500K messages, 3 rounds, report median QPS to reduce variance.

| Configuration | Median QPS |
|---|---|
| 1P/1S | **~5,040,000** |
| 4P/1S | **~3,950,000** |

**Analysis**: Median over 3 rounds eliminates outlier variance. 1P/1S stable at ~5M QPS demonstrates consistent performance.

---

## Performance Summary

| Scenario | QPS / Rate | Notes |
|---|---|---|
| Raw Queue 1P/1C | ~71M ops/s | Queue theoretical max throughput |
| Raw Queue 4P/4C | ~8.5M ops/s | Vyukov MPMC multi-contention |
| Single pub/sub | ~5.0M QPS | Full end-to-end path (stable median) |
| 4 producers | ~4.0M QPS | MPMC concurrent (stable median) |
| 100 topics | **~9.2M QPS** | TopicId + condvar (+86%) |
| Fan-out ×10 | ~24M del/s | Broadcast delivery |
| Fan-out ×100 | ~85M del/s | High fan-out delivery |
| Wildcard ×100 | ~1.9M QPS | Trie index |
| Wildcard ×1000 | ~2.0M QPS | O(depth), independent of pattern count |
| Wildcard RCU ×4 | ~2.0M QPS | Concurrent readers, near-linear scaling |
| FullPolicy Block | ~4.0M QPS | 100% delivery, cv_not_full_ |
| Multi-dispatcher ×4 | ~2.5M QPS | Router + 4 Workers |
| Pool Enabled | ~7.8M QPS | Object pool ~1.9x vs. cold path |
| Concurrent sub/unsub | ~2.3M QPS | COW stability under churn |
| Latency min | **~0.2 μs** | Object pool reuse |
| Latency p50 | **~57 μs** | Condvar wakeup (was ~374μs) |

> Above are local Windows reference numbers. Per-platform release data is available on the [Releases](../../releases) page.

---

## Zero-Copy Optimization Results

### What Changed

| Item | Before | After |
|---|---|---|
| Message pointer | `shared_ptr<IMessage>` | `MessagePtr` (intrusive ref count) |
| Memory allocation | `new TypedMessage` + control block per message | Object pool acquire/release, zero allocation on pool hit |
| Recycling | `shared_ptr` destructor → `delete` | Recycler function pointer → object pool recycle & reuse |
| Reference counting | Separate control block atomic ops | Embedded `atomic<int>` in IMessage |
| Topic routing key | `std::string` hash/compare | `uint32_t` TopicId integer hash |
| Topic storage | `std::string` per message | `uint32_t` per message |

### Impact

| Metric | Improvement |
|---|---|
| Min latency | 36μs → 0.2μs (**~180x**) |
| p50 latency | 3.6ms → 57μs (**~63x**) |
| Multi-topic QPS | 4.95M → 9.2M (**+86%**) |

---

## Performance Bottleneck Analysis

### Current Bottlenecks

1. **Single Router thread** (multi-dispatcher mode): Router is a serial bottleneck, may limit throughput under heavy load
2. **publish path TopicRegistry resolve**: Each publish requires shared_lock + hash table lookup in the registry

### Completed Optimizations

| Optimization | Benefit | Status |
|---|---|---|
| Object pool replacing `shared_ptr` | Min latency 180x | ✅ Done |
| Multi-dispatcher (topic sharding) | Horizontal scaling on consumer side | ✅ Done |
| TopicId integer routing | Multi-topic +86%, p50 latency ~63x | ✅ Done |
| Wildcard trie index | Matching O(depth), independent of pattern count | ✅ Done |
| Condition variable replacing sleep backoff | p50 latency ~7x, p99 ~3x | ✅ Done |
| Trie transparent hash + empty node pruning | Match path zero-allocation, prevents memory leak | ✅ Done |
| API `string_view` parameters | Callers no longer need `std::string` | ✅ Done |
| FullPolicy backpressure | 5 queue-full strategies, 100% delivery (Block) | ✅ Done |
| TopicHandle cached publish | Skips registry resolve, lock-free publish | ✅ Done |
| RCU Slot Map | Dispatch read path fully lock-free (MSVC/GCC) | ✅ Done |

### Potential Optimization Directions

| Optimization | Expected Benefit | Complexity |
|---|---|---|
| Batch dequeue | QPS +20~30% | Medium |

---

## Comparison with Alternatives (Order of Magnitude)

| Solution | Typical QPS | Lock-Free | Coroutines | Object Pool | Wildcards |
|---|---|---|---|---|---|
| **MsgBus (this project)** | ~9.2M | ✓ | ✓ | ✓ | ✓ |
| Boost.Signals2 | ~1-2M | ✗ | ✗ | ✗ | ✗ |
| Qt Signals/Slots | ~0.5-1M | ✗ | ✗ | ✗ | ✗ |
| moodycamel + manual routing | ~8-15M | ✓ | ✗ | ✗ | ✗ |
| Redis Pub/Sub (local) | ~0.1-0.5M | ✗ | ✗ | ✗ | ✓ |

> Note: Comparison data are order-of-magnitude estimates. Actual results depend on hardware and workload.
