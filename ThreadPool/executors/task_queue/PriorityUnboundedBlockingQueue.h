#pragma once

#include <folly/ConstexprMath.h>
#include "Executor.h"
#include <folly/concurrency/PriorityUnboundedQueueSet.h>
#include "executors/task_queue/BlockingQueue.h"
#include <folly/lang/Exception.h>
#include <folly/synchronization/LifoSem.h>

namespace ThreadPool {
    // 不限容量的支持优先级的阻塞队列
    // 主要功能:
    // 使用 PriorityUnboundedQueueSet 作为内部存储,提供多个优先级的队列。
    // 使用 LifoSem 作为信号量实现阻塞获取。
    // 支持 add() 添加元素,可指定优先级。
    // 支持 take() 阻塞获取元素。
    // 提供 try_take()、try_take_for() 非阻塞获取。
    // getNumPriorities() 获取优先级数。
    // size() 获取队列大小。
    // translatePriority() 将业务优先级映射到内部优先级。
    // dequeue() 在获取信号量后出队。
    //
    // 可以配置优先级数量,不会因为容量限制导致获取阻塞。出队顺序按优先级反序

    template<class T, class Semaphore = folly::LifoSem>
    class PriorityUnboundedBlockingQueue : public BlockingQueue<T> {
    public:
        // Note: To use folly::Executor::*_PRI, for numPriorities == 2
        //       MID_PRI and HI_PRI are treated at the same priority level.
        explicit PriorityUnboundedBlockingQueue(
                uint8_t numPriorities,
                const typename Semaphore::Options &semaphoreOptions = {})
                : sem_(semaphoreOptions), queue_(numPriorities) {}

        uint8_t getNumPriorities() override { return queue_.priorities(); }

        // Add at medium priority by default
        BlockingQueueAddResult add(T item) override {
            return addWithPriority(std::move(item), folly::Executor::MID_PRI);
        }

        BlockingQueueAddResult addWithPriority(T item, int8_t priority) override {
            queue_.at_priority(translatePriority(priority)).enqueue(std::move(item));
            return sem_.post();
        }

        T take() override {
            sem_.wait();
            return dequeue();
        }

        folly::Optional<T> try_take() {
            if (!sem_.try_wait()) {
                return folly::none;
            }
            return dequeue();
        }

        folly::Optional<T> try_take_for(std::chrono::milliseconds time) override {
            if (!sem_.try_wait_for(time)) {
                return folly::none;
            }
            return dequeue();
        }

        size_t size() override { return queue_.size(); }

        size_t sizeGuess() const { return queue_.size(); }

    private:
        size_t translatePriority(int8_t const priority) {
            size_t const priorities = queue_.priorities();
            assert(priorities <= 255);
            int8_t const hi = (priorities + 1) / 2 - 1;
            int8_t const lo = hi - (priorities - 1);
            return hi - folly::constexpr_clamp(priority, lo, hi);
        }

        T dequeue() {
            // must follow a successful sem wait
            if (auto obj = queue_.try_dequeue()) {
                return std::move(*obj);
            }
            folly::terminate_with<std::logic_error>("bug in task queue");
        }

        Semaphore sem_;
        folly::PriorityUMPMCQueueSet<T, /* MayBlock = */ true> queue_;
    };

} // namespace ThreadPool
