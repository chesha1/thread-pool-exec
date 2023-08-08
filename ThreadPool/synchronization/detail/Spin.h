/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <thread>

#include <folly/portability/Asm.h>
#include "synchronization/WaitOptions.h"

// 提供了自旋等待的实现
// spin_pause_until 和 spin_yield_until，它们分别实现了两种自旋等待机制

namespace ThreadPool {
    namespace detail {

        enum class spin_result {
            success, // condition passed 自旋等待成功
            timeout, // exceeded deadline 自旋等待超时，等待的资源或条件在给定时间内未能满足
            advance, // exceeded current wait-options component timeout 超过了当前等待选项组件的超时时间
        };

        // 采用自旋等待的通用模板函数。
        // 它在给定的截止时间（deadline）之前持续地检查某个条件（通过调用函数 f 来进行判断）是否为真
        template<typename Clock, typename Duration, typename F>
        spin_result spin_pause_until(
                // Clock：一个时钟类型，用于确定时间点的基础（即 "epoch"），以及时间点的分辨率。
                // 典型的时钟类型包括 std::chrono::system_clock，std::chrono::steady_clock 和 std::chrono::high_resolution_clock
                // Duration：一个持续时间类型，用于定义时间点的分辨率。
                // 例如，如果 Duration 是 std::chrono::milliseconds，那么这个时间点的分辨率就是毫秒级别
                std::chrono::time_point<Clock, Duration> const &deadline,
                WaitOptions const &opt,
                F f) {

            // 如果配置的自旋最大持续时间没有意义（例如，它被设置为 0 或负数），
            // 则函数将提前结束自旋等待
            if (opt.spin_max() <= opt.spin_max().zero()) {
                return spin_result::advance;
            }

            if (f()) {
                return spin_result::success;
            }

            constexpr auto min = std::chrono::time_point<Clock, Duration>::min();
            if (deadline == min) {
                return spin_result::timeout;
            }

            // 自旋等待循环
            // 它使用指定的时钟 Clock 来获取当前时间，并持续检查某个条件 f，直到该条件满足（f() 返回 true），
            // 或者达到某个截止时间 deadline，或者超过了某个最大等待时间 opt.spin_max()
            auto tbegin = Clock::now();
            while (true) {
                if (f()) {
                    return spin_result::success;
                }

                auto const tnow = Clock::now();
                if (tnow >= deadline) {
                    return spin_result::timeout;
                }

                //  Backward time discontinuity in Clock? revise pre_block starting point
                tbegin = std::min(tbegin, tnow);
                if (tnow >= tbegin + opt.spin_max()) {
                    return spin_result::advance;
                }

                //  The pause instruction is the polite way to spin, but it doesn't
                //  actually affect correctness to omit it if we don't have it. Pausing
                //  donates the full capabilities of the current core to its other
                //  hyperthreads for a dozen cycles or so.
                // 告诉处理器当前线程正在等待，允许处理器将其资源重新分配给其他线程或者核心。
                // 这是自旋等待中的 "礼貌" 行为，可以避免处理器在空转时过热
                folly::asm_volatile_pause();
            }
        }

        // 与spin_pause_until不同，这个函数在检查条件时调用了std::this_thread::yield()，
        // 暗示调度器当前线程愿意放弃其时间片，从而允许其它线程运行
        template<typename Clock, typename Duration, typename F>
        spin_result spin_yield_until(
                std::chrono::time_point<Clock, Duration> const &deadline, F f) {
            while (true) {
                if (f()) {
                    return spin_result::success;
                }

                const auto max = std::chrono::time_point<Clock, Duration>::max();
                if (deadline != max && Clock::now() >= deadline) {
                    return spin_result::timeout;
                }

                // 暂停了当前线程的执行并允许操作系统调度其他线程进行执行
                std::this_thread::yield();
            }
        }

    } // namespace detail
} // namespace ThreadPool
