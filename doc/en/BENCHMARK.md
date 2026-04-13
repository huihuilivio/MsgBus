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
- **Optimization**: Object pool + intrusive reference counting (replaces shared_ptr)

---

## Benchmark Scenarios

### 1. Raw Queue — Lock-Free Queue Raw Throughput

**Scenario**: 1 producer / 1 consumer, pure `LockFreeQueue<int>` operations, no message wrapping, no topic routing.

**Purpose**: Measure queue's theoretical maximum throughput as an upper bound reference.

| Metric | Result |
|---|---|
| Messages | 1,000,000 |
| Duration | ~90 ms |
| Throughput | **~11,000,000 ops/s** |

---

### 2. Throughput — Single Producer/Consumer End-to-End

**Scenario**: 1 publisher thread, 1 subscriber handler, full publish→queue→dispatch→handler path.

| Metric | Result |
|---|---|
| Messages | 1,000,000 |
| Duration | ~150 ms |
| Throughput | **~6,700,000 QPS** |

**Analysis**: Overhead vs. Raw Queue mainly comes from:
- Object pool acquire/release (lock-free, minimal)
- `TypedMessage<T>` construction or reset
- Topic hash lookup + shared_mutex read lock
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
| Duration | ~184 ms |
| Throughput | **~5,440,000 QPS** |

**Analysis**: ~20% throughput drop from multi-thread CAS contention, as expected. The Vyukov algorithm maintains good performance under multi-producer load.

---

### 4. Latency — Publish-to-Handler Latency

**Scenario**: 1 publisher, 1 subscriber, measuring per-message latency from publish to handler execution. Includes 1,000 warm-up messages.

| Percentile | After Optimization (μs) | Before Optimization (μs) |
|---|---|---|
| **min** | **0.3** | 36.2 |
| **p50** | 2,196 | 3,582.7 |
| **p90** | 4,977.8 | 4,977.8 |
| **p99** | 5,796.2 | 5,796.2 |
| **max** | 5,910.4 | 5,910.4 |

**Analysis**:

- **Minimum latency dropped from ~36μs to ~0.3μs** (100x improvement), thanks to object pool reuse eliminating heap allocation + intrusive ref counting eliminating shared_ptr control block overhead
- p50 dropped from ~3.6ms to ~2.2ms (~40% improvement), median latency also significantly benefits
- High percentile latencies are dominated by queuing effects (100K messages published back-to-back), object pool optimization has less impact
- In real-world usage (lower publish frequency), latency will approach the min value
- The 3-tier backoff strategy increases latency when idle but significantly reduces CPU usage

---

### 5. Multi-Topic — Multi-Topic Throughput

**Scenario**: 100 different topics, 10,000 messages per topic.

| Metric | After Optimization | Before Optimization |
|---|---|---|
| Topics | 100 | 100 |
| Total messages | 1,000,000 | 1,000,000 |
| Duration | ~172 ms | ~202 ms |
| Throughput | **~5,800,000 QPS** | ~4,950,000 QPS |

**Analysis**: Object pool optimization shows significant gains in multi-topic scenarios (**+18% QPS**), because topic switching causes more message object creation/destruction, amplifying pool reuse benefits.

---

### 6. Fan-out — Broadcast Fan-out

**Scenario**: 1 publisher, N subscribers on the same topic.

#### 10 Subscribers

| Metric | Result |
|---|---|
| Messages | 100,000 |
| Subscribers | 10 |
| Total deliveries | 1,000,000 |
| Duration | ~32 ms |
| Delivery rate | **~31,170,000 deliveries/s** |

#### 100 Subscribers

| Metric | Result |
|---|---|
| Messages | 10,000 |
| Subscribers | 100 |
| Total deliveries | 1,000,000 |
| Duration | ~12 ms |
| Delivery rate | **~84,050,000 deliveries/s** |

**Analysis**: Delivery rate actually increases in fan-out scenarios because:
- Per-message dequeue + topic lookup overhead is amortized across multiple handler calls
- Subscriber list is contiguous in memory (vector), sequential traversal is cache-friendly
- `std::function<void(const T&)>` call overhead is minimal
- Intrusive ref counting: in fan-out scenarios messages are shared by multiple handlers; intrusive ref counting atomic ops are lighter than shared_ptr

---

## Performance Summary

| Scenario | QPS / Rate | Notes |
|---|---|---|
| Raw Queue | ~11M ops/s | Queue theoretical max throughput |
| Single pub/sub | ~6.7M QPS | Full end-to-end path |
| 4 producers | ~5.4M QPS | MPMC concurrent |
| 100 topics | **~5.8M QPS** | Hash routing (object pool +18%) |
| Fan-out ×10 | ~31M del/s | Broadcast delivery |
| Fan-out ×100 | ~84M del/s | High fan-out delivery |
| Latency min | **~0.3 μs** | Object pool reuse (was 36μs) |
| Latency p50 | **~2.2 ms** | Queuing effect dominated (was 3.6ms) |

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

### Impact

| Metric | Improvement |
|---|---|
| Min latency | 36μs → 0.3μs (**~100x**) |
| p50 latency | 3.6ms → 2.2ms (**~40%**) |
| Multi-topic QPS | 4.95M → 5.8M (**+18%**) |
