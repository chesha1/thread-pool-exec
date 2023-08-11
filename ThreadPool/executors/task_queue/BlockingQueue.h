#pragma once

#include <chrono>
#include <exception>
#include <stdexcept>

#include <glog/logging.h>

#include <folly/CPortability.h>
#include <folly/Optional.h>

namespace ThreadPool {

// Some queue implementations (for example, LifoSemMPMCQueue or
// PriorityLifoSemMPMCQueue) support both blocking (BLOCK) and
// non-blocking (THROW) behaviors.
    // 当队列满时阻塞 (BLOCK) 或者抛出异常 (THROW)
    enum class QueueBehaviorIfFull {
        THROW, BLOCK
    };

    // 定义一个异常类：当队列满时抛出该异常
    class FOLLY_EXPORT QueueFullException : public std::runtime_error {
        using std::runtime_error::runtime_error; // Inherit constructors.
    };

    // 结构体用于表示向队列添加数据的结果
    struct BlockingQueueAddResult {
        BlockingQueueAddResult(bool reused = false) : reusedThread(reused) {}

        bool reusedThread;
    };

    template<class T>
    class BlockingQueue {
    public:
        virtual ~BlockingQueue() = default;

        // Adds item to the queue (with priority).
        //
        // Returns true if an existing thread was able to work on it (used
        // for dynamically sizing thread pools), false otherwise.  Return false
        // if this feature is not supported.

        // 添加元素到队列（可能带有优先级）
        //
        // 如果有一个现有的线程可以处理它，返回 true（用于动态调整线程池大小），否则返回 false。
        // 如果不支持此特性，也返回 false。
        virtual BlockingQueueAddResult add(T item) = 0;

        virtual BlockingQueueAddResult addWithPriority(
                T item, int8_t /* priority */) {
            return add(std::move(item));
        }

        virtual uint8_t getNumPriorities() { return 1; }

        // 从队列中取出元素
        virtual T take() = 0;

        // 尝试在指定的时间内从队列中取出元素
        virtual folly::Optional<T> try_take_for(std::chrono::milliseconds time) = 0;

        virtual size_t size() = 0;
    };

} // namespace ThreadPool