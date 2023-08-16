#pragma once

#include "MPMCQueue.h"
#include "executors/task_queue/BlockingQueue.h"
#include <folly/synchronization/LifoSem.h>

namespace ThreadPool {

    template<
            class T,
            // kBehavior 用于描述队列满时的行为，默认是抛出异常；
            QueueBehaviorIfFull kBehavior = QueueBehaviorIfFull::THROW,
            // 信号量
            class Semaphore = folly::LifoSem>
    class LifoSemMPMCQueue : public BlockingQueue<T> {
    public:
        // Note: The queue pre-allocates all memory for max_capacity
        // 构造函数：传入队列的最大容量和信号量选项，这里需要注意队列会预先分配所有内存
        explicit LifoSemMPMCQueue(
                size_t max_capacity,
                const typename Semaphore::Options &semaphoreOptions = {})
                : sem_(semaphoreOptions), queue_(max_capacity) {}

        // 将元素添加到队列中
        BlockingQueueAddResult add(T item) override {
            switch (kBehavior) { // static
                case QueueBehaviorIfFull::THROW:
                    // 如果队列不满，写入元素；如果队列满了，返回 false 并且抛出异常
                    if (!queue_.writeIfNotFull(std::move(item))) {
                        throw QueueFullException("LifoSemMPMCQueue full, can't add item");
                    }
                    break;
                case QueueBehaviorIfFull::BLOCK:
                    // 写入元素
                    queue_.blockingWrite(std::move(item));
                    break;
            }
            // 通知信号量有新的元素已添加到队列
            return sem_.post();
        }

        // 从队列中取出元素
        T take() override {
            T item;
            while (!queue_.readIfNotEmpty(item)) {
                sem_.wait();
            }
            return item;
        }

        // 尝试在指定的时间内从队列中取出元素
        folly::Optional<T> try_take_for(std::chrono::milliseconds time) override {
            T item;

            // 循环尝试从队列中读取元素，只有当读取成功时才退出循环
            while (!queue_.readIfNotEmpty(item)) {

                // 如果信号量在指定的时间内无法获得（即队列为空并且指定时间内没有新的元素添加），
                // 则返回 folly::none，表示没有元素可以取出
                if (!sem_.try_wait_for(time)) {
                    return folly::none;
                }
            }
            return item;
        }

        size_t capacity() { return queue_.capacity(); }

        size_t size() override { return queue_.size(); }

    private:
        // 私有成员：信号量和队列
        Semaphore sem_;

        // 起到了同步和通知的作用
        // 1. 消费者线程同步：当消费者线程尝试从队列中取出元素，
        // 但队列是空的时，该线程会在信号量上等待。
        // 直到有生产者线程将一个新元素放入队列并增加信号量的计数器，
        // 等待的消费者线程才会被唤醒并继续其操作
        // 2. 限制并发访问：信号量也确保了当多个线程尝试从队列中取元素时，
        // 只有一个线程可以成功地取到元素，其它线程会被阻塞直到队列中再次有可用的元素
        ThreadPool::MPMCQueue<T> queue_;
    };

} // namespace ThreadPool
