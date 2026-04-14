#pragma once

/// @file config.h
/// Shared feature detection and tuning constants for MsgBus.

#include <cstddef>

// ---------- Feature detection ----------
// C++20 std::atomic<std::shared_ptr<T>> support.
// Available on MSVC 19.28+, GCC 12+, libstdc++ 12+.
// NOT available on Apple Clang / libc++ as of Xcode 16.
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
#  define MSGBUS_HAS_ATOMIC_SHARED_PTR 1
#else
#  define MSGBUS_HAS_ATOMIC_SHARED_PTR 0
#endif

namespace msgbus {

// ---------- Tuning constants ----------
inline constexpr size_t   kDefaultQueueCapacity = 65536;
inline constexpr size_t   kDefaultPoolCapacity  = 8192;
inline constexpr unsigned kSpinThreshold        = 64;
inline constexpr unsigned kYieldThreshold       = 256;

} // namespace msgbus
