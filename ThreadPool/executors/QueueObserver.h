#pragma once

#include <stdint.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <folly/Function.h>
#include <folly/Portability.h>
#include <folly/Synchronized.h>
#include <folly/portability/SysTypes.h>
#include <folly/io/async/Request.h>

namespace ThreadPool {
    // 观察者提供对于线程启动/停止事件的监听
    // 这对于创建那些，在每个线程中只能有一个的对象，很有用
    // 而且在线程池中添加/删除线程时也能让它们正常工作

    class RequestContext;

/**
 * WorkerProvider is a simple interface that can be used
 * to collect information about worker threads that are pulling work
 * from a given queue.
 */
    // 这是一个接口，用于收集
    // 从指定队列中取出工作的工作线程
    // 的信息
    class WorkerProvider {
    public:
        virtual ~WorkerProvider() {}

        /**
         * Abstract type returned by the collectThreadIds() method.
         * Implementations of the WorkerProvider interface need to define this class.
         * The intent is to return a guard along with a list of worker IDs which can
         * be removed on destruction of this object.
         */
        // 是一个嵌套的抽象类，设计意图是返回一个 keep-alive 及一组工作线程 ID，
        // 当这个对象被销毁时，这些 ID 将被移除
        class KeepAlive {
        public:
            virtual ~KeepAlive() = 0;
        };

        // collectThreadIds() will return this aggregate type which includes an
        // instance of the WorkersGuard.
        struct IdsWithKeepAlive {
            std::unique_ptr<KeepAlive> guard;
            std::vector<pid_t> threadIds;
        };

        // Capture the Thread IDs of all threads consuming from a given queue.
        // The provided vector should be populated with the OS Thread IDs and the
        // method should return a SharedMutex which the caller can lock.
        // 捕获从给定队列中取出工作的所有线程的 ID
        virtual IdsWithKeepAlive collectThreadIds() = 0;
    };

    class ThreadIdWorkerProvider : public WorkerProvider {
    public:

        IdsWithKeepAlive collectThreadIds() override final;

        // 向 osThreadIds_ 集合中添加线程ID
        void addTid(pid_t tid);

        // Will block until all KeepAlives have been destroyed, if any exist
        // 从 osThreadIds_ 集合中删除线程ID
        void removeTid(pid_t tid);

    private:
        // 使用 synchronized 模板进行同步的一个无序集合，
        // 用于存储操作系统线程 ID
        folly::Synchronized<std::unordered_set<pid_t>> osThreadIds_;

        // 用于线程退出时的互斥锁
        folly::SharedMutex threadsExitMutex_;
    };

    // 这是一个接口，定义了队列中对象被入队和出队时的操作
    class QueueObserver {
    public:
        virtual ~QueueObserver() {}

//        virtual intptr_t onEnqueued(const RequestContext *) = 0;
        virtual intptr_t onEnqueued(const folly::RequestContext *) = 0;

        virtual void onDequeued(intptr_t) = 0;
    };

    // 这是一个接口，定义了创建 QueueObserver 的方法
    class QueueObserverFactory {
    public:
        virtual ~QueueObserverFactory() {}

        virtual std::unique_ptr<QueueObserver> create(int8_t pri) = 0;

        static std::unique_ptr<QueueObserverFactory> make(
                const std::string &context,
                size_t numPriorities,
                WorkerProvider *workerProvider = nullptr);
    };

    using MakeQueueObserverFactory = std::unique_ptr<QueueObserverFactory>(
            const std::string &, size_t, WorkerProvider *);
#if FOLLY_HAVE_WEAK_SYMBOLS
    FOLLY_ATTR_WEAK MakeQueueObserverFactory make_queue_observer_factory;
#else
    constexpr MakeQueueObserverFactory *make_queue_observer_factory = nullptr;
#endif

    struct QueueInfo {
        std::unordered_map<std::string, std::unordered_set<pid_t>> namesAndTids;
        std::vector<std::unique_ptr<WorkerProvider::KeepAlive>> keepAlives;
    };
    using LaggingQueueInfoFunc = folly::Function<QueueInfo()>;
} // namespace ThreadPool

