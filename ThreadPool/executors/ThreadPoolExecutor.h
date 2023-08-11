#pragma once

#include <algorithm>
#include <mutex>
#include <queue>

#include <glog/logging.h>

#include "DefaultKeepAliveExecutor.h"

#include <folly/Memory.h>
#include <folly/SharedMutex.h>
#include <folly/executors/GlobalThreadPoolList.h>

#include "executors/task_queue/LifoSemMPMCQueue.h"
#include "executors/thread_factory/NamedThreadFactory.h"

#include <folly/io/async/Request.h>
#include <folly/portability/GFlags.h>
#include <folly/synchronization/AtomicStruct.h>

#include "synchronization/Baton.h"

namespace ThreadPool {

/* Base class for implementing threadpool based executors.
 *
 * Dynamic thread behavior:
 *
 * ThreadPoolExecutors may vary their actual running number of threads
 * between minThreads_ and maxThreads_, tracked by activeThreads_.
 * The actual implementation of joining an idle thread is left to the
 * ThreadPoolExecutors' subclass (typically by LifoSem try_take_for
 * timing out).  Idle threads should be removed from threadList_, and
 * threadsToJoin incremented, and activeThreads_ decremented.
 *
 * On task add(), if an executor can guarantee there is an active
 * thread that will handle the task, then nothing needs to be done.
 * If not, then ensureActiveThreads() should be called to possibly
 * start another pool thread, up to maxThreads_.
 *
 * ensureJoined() is called on add(), such that we can join idle
 * threads that were destroyed (which can't be joined from
 * themselves).
 *
 * Thread pool stats accounting:
 *
 * Derived classes must register instances to keep stats on all thread
 * pools by calling registerThreadPoolExecutor(this) on constructions
 * and deregisterThreadPoolExecutor(this) on destruction.
 *
 * Registration must be done wherever getPendingTaskCountImpl is implemented
 * and getPendingTaskCountImpl should be marked 'final' to avoid data races.
 */
    // ThreadPoolExecutor 类定义：
    // 基础定义：该类从DefaultKeepAliveExecutor公共继承而来。
    // 构造与析构：有一个构造函数用于初始化线程池的最大线程数、最小线程数和线程工厂；有一个析构函数用于销毁线程池。
    // 线程工厂：提供设置和获取线程工厂的方法。
    // 线程数量：提供查询和设置线程数量的方法。
    // 线程操作：stop和join方法提供停止和加入线程的功能。
    // 统计信息：定义了用于获取线程池统计信息的结构和方法。
    // 任务统计：定义了用于获取任务统计信息的结构和方法。
    // 线程句柄和观察者：定义了线程句柄的基类以及一个观察者接口，用于在线程启动和停止时执行操作。
    // 内部线程和任务结构：定义了表示线程和任务的内部结构。
    // 核心逻辑：该类的核心逻辑涉及如何添加线程、删除线程、运行任务和停止线程。
    // 线程列表和停止线程队列：这两个类提供了对当前活跃线程的管理和对已停止线程的管理。
    // 动态线程调整：提供了一组函数和变量，用于确保线程池中有足够的活跃线程。
    class ThreadPoolExecutor : public DefaultKeepAliveExecutor {
    public:
        // 需要最大线程数、最小线程数和一个线程工厂
        explicit ThreadPoolExecutor(
                size_t maxThreads,
                size_t minThreads,
                std::shared_ptr<ThreadFactory> threadFactory);

        ~ThreadPoolExecutor() override;

        // 将一个函数添加到线程池队列中执行
        // On task add(), if an executor ...
        void add(Func func) override = 0;

        virtual void add(
                Func func, std::chrono::milliseconds expiration, Func expireCallback);

        void setThreadFactory(std::shared_ptr<ThreadFactory> threadFactory) {
            CHECK(numThreads() == 0);
            threadFactory_ = std::move(threadFactory);
        }

        std::shared_ptr<ThreadFactory> getThreadFactory() const {
            return threadFactory_;
        }

        size_t numThreads() const;

        // 动态地设置线程池中的线程数量
        void setNumThreads(size_t numThreads);

        // Return actual number of active threads -- this could be different from
        // numThreads() due to ThreadPoolExecutor's dynamic behavior.
        size_t numActiveThreads() const;

        /*
         * stop() is best effort - there is no guarantee that unexecuted tasks won't
         * be executed before it returns. Specifically, IOThreadPoolExecutor's stop()
         * behaves like join().
         */

        // 停止线程池和等待所有线程完成
        void stop();

        // 停止并 join 线程池中的所有线程
        void join();

        /**
         * Execute f against all ThreadPoolExecutors, primarily for retrieving and
         * exporting stats.
         */
        // 对所有的 ThreadPoolExecutors 执行一个函数
        static void withAll(folly::FunctionRef<void(ThreadPoolExecutor &)> f);

        // 线程池的统计信息
        struct PoolStats {
            PoolStats()
                    : threadCount(0),
                      idleThreadCount(0),
                      activeThreadCount(0),
                      pendingTaskCount(0),
                      totalTaskCount(0),
                      maxIdleTime(0) {}

            size_t threadCount, idleThreadCount, activeThreadCount;
            uint64_t pendingTaskCount, totalTaskCount;
            std::chrono::nanoseconds maxIdleTime;
        };

        PoolStats getPoolStats() const;

        size_t getPendingTaskCount() const;

        const std::string &getName() const;

        /**
         * Return the cumulative CPU time used by all threads in the pool, including
         * those that are no longer alive. Requires system support for per-thread CPU
         * clocks. If not available, the function returns 0. This operation can be
         * expensive.
         */
        std::chrono::nanoseconds getUsedCpuTime() const {
            folly::SharedMutex::ReadHolder r{&threadListLock_};
            return threadList_.getUsedCpuTime();
        }

        // 任务统计
        struct TaskStats {
            TaskStats() : expired(false), waitTime(0), runTime(0), requestId(0) {}

            bool expired;
            std::chrono::nanoseconds waitTime;
            std::chrono::nanoseconds runTime;
            std::chrono::steady_clock::time_point enqueueTime;
            uint64_t requestId;
        };

        using TaskStatsCallback = std::function<void(const TaskStats &)>;

        void subscribeToTaskStats(TaskStatsCallback cb);

        /**
         * Base class for threads created with ThreadPoolExecutor.
         * Some subclasses have methods that operate on these
         * handles.
         */
        // 由 ThreadPoolExecutor 创建的线程的基类
        class ThreadHandle {
        public:
            virtual ~ThreadHandle() = default;
        };

        /**
         * Observer interface for thread start/stop.
         * Provides hooks so actions can be taken when
         * threads are created
         */
        // 观察线程的启动和停止。
        // 提供了 Hook，以便在线程创建或停止时采取操作
        class Observer {
        public:
            // 当线程启动时被调用的虚拟方法
            virtual void threadStarted(ThreadHandle *) = 0;

            // 当线程停止时被调用的虚拟方法
            virtual void threadStopped(ThreadHandle *) = 0;

            // 当线程在之前已经启动时被调用的方法，默认调用 threadStarted
            virtual void threadPreviouslyStarted(ThreadHandle *h) { threadStarted(h); }

            // 当线程尚未停止时被调用的方法，默认调用 threadStopped
            virtual void threadNotYetStopped(ThreadHandle *h) { threadStopped(h); }

            virtual ~Observer() = default;
        };

        virtual void addObserver(std::shared_ptr<Observer>);

        virtual void removeObserver(std::shared_ptr<Observer>);

        void setThreadDeathTimeout(std::chrono::milliseconds timeout) {
            threadTimeout_ = timeout;
        }

    protected:
        // Prerequisite: threadListLock_ writelocked
        void addThreads(size_t n);

        // Prerequisite: threadListLock_ writelocked
        void removeThreads(size_t n, bool isJoin);

        struct TaskStatsCallbackRegistry;

        struct //
            // 对齐到 folly 定义的缓存行大小
        alignas(folly::cacheline_align_v) //
        alignas(folly::AtomicStruct<std::chrono::steady_clock::time_point>) //
        Thread:
    public ThreadHandle {
            explicit Thread(ThreadPoolExecutor
            *pool)
            // ID自增
            : id(nextId++),
                    // 默认初始化线程句柄
                    handle(),
                    // 初始设置为空闲状态
                    idle(true),
                    // 记录线程最后活跃的时间
                    lastActiveTime(std::chrono::steady_clock::now()),
                    // 初始化任务统计回调
                    taskStatsCallbacks(pool->taskStatsCallbacks_)
            {}

            ~Thread()
            override =
            default;

            // // 用于获取线程已使用的 CPU 时间的函数
            std::chrono::nanoseconds usedCpuTime() const;

            static std::atomic<uint64_t> nextId;
            uint64_t id;
            std::thread handle;

            // 表示线程是否处于空闲状态的原子变量
            std::atomic<bool> idle;
            folly::AtomicStruct<std::chrono::steady_clock::time_point> lastActiveTime;

            // 一个 Baton 对象，用于线程同步
            Baton<> startupBaton;
            std::shared_ptr<TaskStatsCallbackRegistry> taskStatsCallbacks;
        };

        typedef std::shared_ptr<Thread> ThreadPtr;

        struct Task {
            // 任务的过期信息
            struct Expiration {
                std::chrono::milliseconds expiration;
                Func expireCallback;
            };

            Task(
                    // 要执行的函数
                    Func &&func,
                    // 任务的过期时间
                    std::chrono::milliseconds expiration,
                    // 任务过期时的回调函数
                    Func &&expireCallback);

            Func func_;

            // 任务入队的时间
            std::chrono::steady_clock::time_point enqueueTime_;
            // 与此任务关联的请求上下文
            std::shared_ptr<folly::RequestContext> context_;
            std::unique_ptr<Expiration> expiration_;
        };

        // 接收一个 Thread 指针和一个 Tas k对象
        void runTask(const ThreadPtr &thread, Task &&task);

        // The function that will be bound to pool threads. It must call
        // thread->startupBaton.post() when it's ready to consume work.
        // 这个函数会被绑定到线程池中的线程上。
        // 这意味着当线程池中的线程开始运行时，它会执行这个函数。
        // 当线程准备好开始处理工作时，它必须调用 thread->startupBaton.post()
        virtual void threadRun(ThreadPtr thread) = 0;

        // Stop n threads and put their ThreadPtrs in the stoppedThreads_ queue
        // and remove them from threadList_, either synchronize or asynchronize
        // Prerequisite: threadListLock_ writelocked
        // 这个函数的目的是停止 n 个线程，
        // 并将它们的 ThreadPtr 放入 stoppedThreads_ 队列中
        // 同时，这些线程会从 threadList_ 中移除。这可能是同步的也可能是异步的
        virtual void stopThreads(size_t n) = 0;

        // Join n stopped threads and remove them from waitingForJoinThreads_ queue.
        // Should not hold a lock because joining thread operation may invoke some
        // cleanup operations on the thread, and those cleanup operations may
        // require a lock on ThreadPoolExecutor.
        void joinStoppedThreads(size_t n);

        // Create a suitable Thread struct
        virtual ThreadPtr makeThread() { return std::make_shared<Thread>(this); }

        static void registerThreadPoolExecutor(ThreadPoolExecutor *tpe);

        static void deregisterThreadPoolExecutor(ThreadPoolExecutor *tpe);

        // Prerequisite: threadListLock_ readlocked or writelocked
        virtual size_t getPendingTaskCountImpl() const = 0;

        // 管理 Thread 对象的列表
        class ThreadList {
        public:
            // 将给定的 ThreadPtr 对象（即线程指针）添加到 vec_ 向量中，
            // 并确保该向量仍然是按线程 ID 排序的
            void add(const ThreadPtr &state) {
                auto it = std::lower_bound(vec_.begin(), vec_.end(), state, Compare{});
                vec_.insert(it, state);
            }

            void remove(const ThreadPtr &state) {
                auto itPair =
                        std::equal_range(vec_.begin(), vec_.end(), state, Compare{});
                CHECK(itPair.first != vec_.end());
                CHECK(std::next(itPair.first) == itPair.second);
                vec_.erase(itPair.first);
                pastCpuUsed_ += state->usedCpuTime();
            }

            // 检查给定的 ThreadPtr 对象是否存在于 vec_ 向量中，使用二分查找
            bool contains(const ThreadPtr &ts) const {
                return std::binary_search(vec_.cbegin(), vec_.cend(), ts, Compare{});
            }

            const std::vector<ThreadPtr> &get() const { return vec_; }

            // 返回由 vec_ 向量中的所有线程以及以前已经死亡的线程所使用的总 CPU 时间
            std::chrono::nanoseconds getUsedCpuTime() const {
                auto acc{pastCpuUsed_};
                for (const auto &thread: vec_) {
                    acc += thread->usedCpuTime();
                }
                return acc;
            }

        private:
            // 如何比较两个 ThreadPtr 对象。
            // 这里是基于它们的id进行比较的
            struct Compare {
                bool operator()(const ThreadPtr &ts1, const ThreadPtr &ts2) const {
                    return ts1->id < ts2->id;
                }
            };

            // 一个存储 ThreadPtr 对象的向量
            std::vector<ThreadPtr> vec_;
            // cpu time used by threads that are no longer alive
            // 一个表示以前已经死亡的线程所使用的 CPU 时间的变量
            std::chrono::nanoseconds pastCpuUsed_{0};
        };

        class StoppedThreadQueue : public BlockingQueue<ThreadPtr> {
        public:
            BlockingQueueAddResult add(ThreadPtr item) override;

            ThreadPtr take() override;

            size_t size() override;

            folly::Optional<ThreadPtr> try_take_for(
                    std::chrono::milliseconds /*timeout */) override;

        private:
            folly::LifoSem sem_;
            std::mutex mutex_;
            std::queue<ThreadPtr> queue_;
        };

        std::shared_ptr<ThreadFactory> threadFactory_;

        ThreadList threadList_;
        folly::SharedMutex threadListLock_;
        StoppedThreadQueue stoppedThreads_;
        std::atomic<bool> isJoin_{false}; // whether the current downsizing is a join

        struct TaskStatsCallbackRegistry {
            folly::ThreadLocal<bool> inCallback;
            folly::Synchronized<std::vector<TaskStatsCallback>> callbackList;
        };
        std::shared_ptr<TaskStatsCallbackRegistry> taskStatsCallbacks_;
        std::vector<std::shared_ptr<Observer>> observers_;
        folly::ThreadPoolListHook threadPoolHook_;

        // Dynamic thread sizing functions and variables
        void ensureMaxActiveThreads();

        void ensureActiveThreads();

        void ensureJoined();

        bool minActive();

        bool tryTimeoutThread();

        // These are only modified while holding threadListLock_, but
        // are read without holding the lock.
        std::atomic<size_t> maxThreads_{0};
        std::atomic<size_t> minThreads_{0};
        std::atomic<size_t> activeThreads_{0};

        std::atomic<size_t> threadsToJoin_{0};
        std::atomic<std::chrono::milliseconds> threadTimeout_;

        void joinKeepAliveOnce() {
            if (!std::exchange(keepAliveJoined_, true)) {
                joinKeepAlive();
            }
        }

        bool keepAliveJoined_{false};
    };

} // namespace ThreadPool
