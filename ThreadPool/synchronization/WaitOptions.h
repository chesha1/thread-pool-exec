#pragma once

#include <chrono>

#include <folly/CPortability.h>

namespace ThreadPool {

/// WaitOptions
///
/// Various synchronization primitives as well as various concurrent data
/// structures built using them have operations which might wait. This type
/// represents a set of options for controlling such waiting.
    class WaitOptions {
        // 该类表示用于控制等待行为的一组选项。
        // 它适用于各种同步原语以及使用它们构建的并发数据结构
    public:
        struct Defaults {
            // 这个嵌套结构体定义了等待选项的默认值

            /// spin_max
            ///
            /// If multiple threads are actively using a synchronization primitive,
            /// whether indirectly via a higher-level concurrent data structure or
            /// directly, where the synchronization primitive has an operation which
            /// waits and another operation which wakes the waiter, it is common for
            /// wait and wake events to happen almost at the same time. In this state,
            /// we lose big 50% of the time if the wait blocks immediately.
            ///
            /// We can improve our chances of being waked immediately, before blocking,
            /// by spinning for a short duration, although we have to balance this
            /// against the extra cpu utilization, latency reduction, power consumption,
            /// and priority inversion effect if we end up blocking anyway.
            ///
            /// We use a default maximum of 2 usec of spinning. As partial consolation,
            /// since spinning as implemented in folly uses the pause instruction where
            /// available, we give a small speed boost to the colocated hyperthread.
            ///
            /// On circa-2013 devbox hardware, it costs about 7 usec to FUTEX_WAIT and
            /// then be awoken. Spins on this hw take about 7 nsec, where all but 0.5
            /// nsec is the pause instruction.

            // 定义了在阻塞之前进行自旋等待的最长时间
            // 在此代码中，自旋等待的最长时间被设置为2微秒
            static constexpr std::chrono::nanoseconds spin_max =
                    std::chrono::microseconds(2);
            // 表示是否启用日志记录
            static constexpr bool logging_enabled = true;
        };

        // 返回当前的自旋等待最长时间
        constexpr std::chrono::nanoseconds spin_max() const { return spin_max_; }

        // 设置自旋等待的最长时间，并返回对对象的引用以便进行链接调用
        constexpr WaitOptions &spin_max(std::chrono::nanoseconds dur) {
            spin_max_ = dur;
            return *this;
        }

        // 返回当前是否启用了日志记录
        constexpr bool logging_enabled() const { return logging_enabled_; }

        // 启用或禁用日志记录，并返回对对象的引用以便进行链接调用
        constexpr WaitOptions &logging_enabled(bool enable) {
            logging_enabled_ = enable;
            return *this;
        }

    private:
        // 存储自旋等待的最长时间
        // 默认值从 Defaults 结构体中获得。
        std::chrono::nanoseconds spin_max_ = Defaults::spin_max;
        bool logging_enabled_ = Defaults::logging_enabled;
    };

} // namespace ThreadPool
