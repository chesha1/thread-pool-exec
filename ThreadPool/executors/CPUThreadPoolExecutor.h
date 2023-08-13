#pragma once

#include <limits.h>

#include <array>

#include "executors/QueueObserver.h"
#include "executors/ThreadPoolExecutor.h"

FOLLY_GFLAGS_DECLARE_bool(dynamic_cputhreadpoolexecutor);

namespace ThreadPool {

/**
 * A Thread pool for CPU bound tasks.
 *
 * @note A single queue backed by folly/LifoSem and folly/MPMC queue.
 * Because of this contention can be quite high,
 * since all the worker threads and all the producer threads hit
 * the same queue. MPMC queue excels in this situation but dictates a max queue
 * size.
 *
 * @note The default queue throws when full (folly::QueueBehaviorIfFull::THROW),
 * so add() can fail. Furthermore, join() can also fail if the queue is full,
 * because it enqueues numThreads poison tasks to stop the threads. If join() is
 * needed to be guaranteed to succeed PriorityLifoSemMPMCQueue can be used
 * instead, initializing the lowest priority's (LO_PRI) capacity to at least
 * numThreads. Poisons use LO_PRI so if that priority is not used for any user
 * task join() is guaranteed not to encounter a full queue.
 *
 * @note If a blocking queue (folly::QueueBehaviorIfFull::BLOCK) is used, and
 * tasks executing on a given thread pool schedule more tasks, deadlock is
 * possible if the queue becomes full.  Deadlock is also possible if there is
 * a circular dependency among multiple thread pools with blocking queues.
 * To avoid this situation, use non-blocking queue(s), or schedule tasks only
 * from threads not belonging to the given thread pool(s), or use
 * folly::IOThreadPoolExecutor.
 *
 * @note LifoSem wakes up threads in Lifo order - i.e. there are only few
 * threads as necessary running, and we always try to reuse the same few threads
 * for better cache locality.
 * Inactive threads have their stack madvised away. This works quite well in
 * combination with Lifosem - it almost doesn't matter if more threads than are
 * necessary are specified at startup.
 *
 * @note Supports priorities - priorities are implemented as multiple queues -
 * each worker thread checks the highest priority queue first. Threads
 * themselves don't have priorities set, so a series of long running low
 * priority tasks could still hog all the threads. (at last check pthreads
 * thread priorities didn't work very well).
 */
    // 主要功能:
    // 1. 创建CPU绑定的线程池,用于执行CPU密集型任务。
    // 2. 使用 folly::LifoSem 作为同步原语,以获得更好的缓存局部性。线程以 LIFO 顺序唤醒,重用最后退出的线程,从而提高缓存命中率。
    // 3. 支持优先级队列。使用多个队列(每个优先级一个队列)实现任务优先级,高优先级队列会先被 worker 线程检查
    // 4. 使用 folly::MPMC 队列作为任务队列,以提高多个生产者线程同时添加任务时的性能。
    //    MPMC 队列在这种场景下效果很好,但需要限制最大队列大小
    // 5. 采用延迟释放线程的方式管理线程数。允许指定最大/最小线程数,线程空闲超时后会自动退出,任务到来时再重新创建
    // 6. 支持添加队列观察器,观察任务的入队和出队事件
    // 7. 支持限制线程池内线程进行阻塞调用
    class CPUThreadPoolExecutor : public ThreadPoolExecutor {
    public:
        struct CPUTask;

        // 配置某个组件的参数
        struct Options {
            enum class Blocking {
                prohibit,
                allow,
            };

            constexpr Options() noexcept
                    : blocking{Blocking::allow}, queueObserverFactory{nullptr} {}

            Options &setBlocking(Blocking b) {
                blocking = b;
                return *this;
            }

            Options &setQueueObserverFactory(
                    std::unique_ptr<ThreadPool::QueueObserverFactory> factory) {
                queueObserverFactory = std::move(factory);
                return *this;
            }

            Blocking blocking;
            std::unique_ptr<ThreadPool::QueueObserverFactory> queueObserverFactory{nullptr};
        };

        // These function return unbounded blocking queues with the default semaphore
        // (LifoSem).
        static std::unique_ptr<BlockingQueue<CPUTask>> makeDefaultQueue();

        static std::unique_ptr<BlockingQueue<CPUTask>> makeDefaultPriorityQueue(
                int8_t numPriorities);

        // These function return unbounded blocking queues with ThrottledLifoSem.
        // 返回一个无界的阻塞队列 BlockingQueue<CPUTask>
        // 使用 ThrottledLifoSem 作为队列的阻塞策略
        // wakeUpInterval 参数指定ThrottledLifoSem 的唤醒时间间隔,默认为空
        static std::unique_ptr<BlockingQueue<CPUTask>> makeThrottledLifoSemQueue(
                std::chrono::nanoseconds wakeUpInterval = {});

        // 返回一个无界的阻塞优先级队列 BlockingQueue<CPUTask>
        static std::unique_ptr<BlockingQueue<CPUTask>>
        makeThrottledLifoSemPriorityQueue(
                int8_t numPriorities, std::chrono::nanoseconds wakeUpInterval = {});

        // 各个版本的构造函数
        // numThreads: 线程池的固定线程数目
        // numThreads.first: 线程池的最小线程数
        // numThreads.second: 线程池的最大线程数
        // taskQueue: 任务队列,典型的是各种 BlockingQueue
        // threadFactory: 线程工厂
        // numPriorities: 优先级数量,用于优先级队列
        // maxQueueSize: 队列最大长度,用于有容量限制的队列
        // opt: 配置选项
        CPUThreadPoolExecutor(
                size_t numThreads,
                std::unique_ptr<BlockingQueue<CPUTask>> taskQueue,
                std::shared_ptr<ThreadFactory> threadFactory =
                std::make_shared<NamedThreadFactory>("CPUThreadPool"),
                Options opt = {});

        CPUThreadPoolExecutor(
                std::pair<size_t, size_t> numThreads,
                std::unique_ptr<BlockingQueue<CPUTask>> taskQueue,
                std::shared_ptr<ThreadFactory> threadFactory =
                std::make_shared<NamedThreadFactory>("CPUThreadPool"),
                Options opt = {});

        explicit CPUThreadPoolExecutor(size_t numThreads, Options opt = {});

        CPUThreadPoolExecutor(
                size_t numThreads,
                std::shared_ptr<ThreadFactory> threadFactory,
                Options opt = {});

        explicit CPUThreadPoolExecutor(
                std::pair<size_t, size_t> numThreads,
                std::shared_ptr<ThreadFactory> threadFactory =
                std::make_shared<NamedThreadFactory>("CPUThreadPool"),
                Options opt = {});

        CPUThreadPoolExecutor(
                size_t numThreads,
                int8_t numPriorities,
                std::shared_ptr<ThreadFactory> threadFactory =
                std::make_shared<NamedThreadFactory>("CPUThreadPool"),
                Options opt = {});

        CPUThreadPoolExecutor(
                size_t numThreads,
                int8_t numPriorities,
                size_t maxQueueSize,
                std::shared_ptr<ThreadFactory> threadFactory =
                std::make_shared<NamedThreadFactory>("CPUThreadPool"),
                Options opt = {});

        ~CPUThreadPoolExecutor() override;

        void add(Func func) override;

        void add(
                Func func,
                std::chrono::milliseconds expiration,
                Func expireCallback = nullptr) override;

        void addWithPriority(Func func, int8_t priority) override;

        // func: 要执行的任务函数
        // priority: 任务优先级
        // expiration: 任务的过期时间
        // expireCallback: 任务过期时的回调函数
        virtual void add(
                Func func,
                int8_t priority,
                std::chrono::milliseconds expiration,
                Func expireCallback = nullptr);

        size_t getTaskQueueSize() const;

        uint8_t getNumPriorities() const override;

        // CPUTask 类封装了一个任务,保存了任务函数、优先级、到期时间等信息。
        // 还可以保存一个队列观察器的 payload ,在出队时传递给观察器
        struct CPUTask : public ThreadPoolExecutor::Task {
            CPUTask(
                    Func &&f,
                    std::chrono::milliseconds expiration,
                    Func &&expireCallback,
                    int8_t pri)
                    : Task(std::move(f), expiration, std::move(expireCallback)),
                      priority_(pri) {}

            CPUTask()
                    : Task(nullptr, std::chrono::milliseconds(0), nullptr),
                      poison(true),
                      priority_(0) {}

            size_t queuePriority() const { return priority_; }

            intptr_t &queueObserverPayload() { return queueObserverPayload_; }

            bool poison = false;

        private:
            int8_t priority_;
            intptr_t queueObserverPayload_;
        };

        static const size_t kDefaultMaxQueueSize;

    protected:
        BlockingQueue<CPUTask> *getTaskQueue();

        // threadIdWorkerProvider 这个类主要是用来收集线程 id 和执行该线程的 worker 之间的映射关系。
        // 之所以需要收集这个映射关系,是因为在使用 thread pool 时,我们提交的任务是通过 pool 的 executor 来调度执行的,
        // 而实际的执行是在 pool 内部的某个 worker 线程上执行。
        // 但是从 executor 的角度来看,它只知道提交了一个任务,但不知道实际会在哪个线程上执行该任务。
        // 有的时候需要获取执行任务的线程 id ,比如用于关联日志或调试。
        // 这时就需要使用 threadIdWorkerProvider 建立的映射关系来获取。
        // 具体使用方式是:
        // 1. 在每个 worker 线程开始处理任务前调用 provider 的 registerThread() 方法注册线程 id
        // 2. 在需要获取线程 id 时,
        // 使用 provider 的 getThreadIdForCaller() 方法传入执行任务的执行上下文环境即可获取对应的线程 id
        // 3. 在线程退出前调用 provider 的 unregisterThread() 方法取消注册
        //
        // 所以在 thread pool 实现中持有一个 threadIdWorkerProvider,
        // 可以方便地建立线程 id 和执行线程的映射,从而在需要时获取执行任务的线程 id。
        // 这个 threadIdCollector_ 字段就是用于持有这个 provider 对象的。
        std::unique_ptr<ThreadIdWorkerProvider> threadIdCollector_{
                std::make_unique<ThreadIdWorkerProvider>()};

    private:
        void threadRun(ThreadPtr thread) override;

        //停止线程池中的 n 个线程
        void stopThreads(size_t n) override;

        size_t getPendingTaskCountImpl() const override final;

        // 是否正在尝试减少需要停止的线程数
        bool tryDecrToStop();

        // 是否应该停止当前工作线程
        bool taskShouldStop(folly::Optional<CPUTask> &);

        template<bool withPriority>
        void addImpl(
                Func func,
                int8_t priority,
                std::chrono::milliseconds expiration,
                Func expireCallback);

        std::unique_ptr<ThreadPool::QueueObserverFactory> createQueueObserverFactory();

        // 获取指定优先级任务队列的 QueueObserver 对象
        QueueObserver *FOLLY_NULLABLE getQueueObserver(int8_t pri);

        // 默认实现是无界队列(UnboundedBlockingQueue),也支持有界优先级队列(PriorityLifoSemMPMCQueue)
        // 队列使用了 LifoSem 做同步,可以最小化线程唤醒数,提高缓存局部性
        // 还可以用 ThrottledLifoSem 来节流线程唤醒,降低CPU使用率
        std::unique_ptr<BlockingQueue<CPUTask>> taskQueue_;
        // It is possible to have as many detectors as there are priorities,
        std::array<std::atomic<ThreadPool::QueueObserver *>, UCHAR_MAX + 1> queueObservers_;
        std::unique_ptr<ThreadPool::QueueObserverFactory> queueObserverFactory_;
        std::atomic<ssize_t> threadsToStop_{0};
        Options::Blocking prohibitBlockingOnThreadPools_ = Options::Blocking::allow;

        // CPUThreadPoolExecutor 的主要成员变量包括:
        // threadListLock_ - 保护线程列表的共享互斥量
        // threadList_ - 存活线程的列表
        // stoppedThreads_ - 已停止线程的列表
        // threadsToStop_ - 计数器,需要停止的线程数
        // taskQueue_ - 任务队列,一般为阻塞队列
        // threadIdCollector_ - 用于收集线程ID映射的对象
        // threadTimeout_ - 线程空闲超时时间
        // maxQueueSize_ - 队列最大容量大小
        // minThreads_ - 最小线程数
        // maxThreads_ - 最大线程数
        // activeThreads_ - 当前活跃线程数
        // queueObserverFactory_ - 队列观察器工厂
        // queueObservers_ - 各优先级队列的观察器
        // observers_ - 观察器列表
        // threadPoolHook_ - 线程池钩子函数
        // prohibitBlockingOnThreadPools_ - 是否禁止阻塞调用

        // 这些成员变量用于实现线程池的关键功能:
        // 线程管理
        // 任务队列
        // 队列监控
        // 线程生命周期钩子
        // 阻塞调用控制等
    };

} // namespace ThreadPool

