#pragma once

#include <folly/concurrency/UnboundedQueue.h>
#include "executors/task_queue/BlockingQueue.h"
#include <folly/synchronization/LifoSem.h>

namespace ThreadPool {
    // 不限容量的阻塞队列
    // 主要功能:
    // 使用 UMPMCQueue 作为无界队列存储。
    // 使用 LifoSem 作为信号量实现阻塞获取。
    // 提供 add() 方法入队。
    // 提供 take() 方法阻塞获取。
    // 提供 try_take_for() 方法非阻塞获取。
    // size() 方法获取队列大小。

    // 这样通过组合无界队列和信号量,实现一个阻塞的无界队列,主要特点是:
    // 入队永不阻塞,出队在为空时阻塞等待。
    // 支持限时的非阻塞出队 try_take_for()。
    // 无容量限制,永不填满。
    // 简单易用。
    //
    // 适用于需要无大小限制和阻塞获取语义的场景,比如处理持续变化的流式数据。

    template<class T, class Semaphore = folly::LifoSem>
    class UnboundedBlockingQueue : public BlockingQueue<T> {
    public:
        explicit UnboundedBlockingQueue(
                const typename Semaphore::Options &semaphoreOptions = {})
                : sem_(semaphoreOptions) {}

        BlockingQueueAddResult add(T item) override {
            queue_.enqueue(std::move(item));
            return sem_.post();
        }

        T take() override {
            sem_.wait();
            return queue_.dequeue();
        }

        folly::Optional<T> try_take_for(std::chrono::milliseconds time) override {
            if (!sem_.try_wait_for(time)) {
                return folly::none;
            }
            return queue_.dequeue();
        }

        size_t size() override { return queue_.size(); }

    private:
        Semaphore sem_;
        folly::UMPMCQueue<T, false, 6> queue_;
    };

} // namespace ThreadPool
