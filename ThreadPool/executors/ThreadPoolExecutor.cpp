#include "executors/ThreadPoolExecutor.h"
#include <ctime>

#include <folly/executors/GlobalThreadPoolList.h>
#include <folly/portability/PThread.h>
#include <folly/synchronization/AsymmetricThreadFence.h>
#include <folly/tracing/StaticTracepoint.h>

namespace ThreadPool {

    // 线程安全的访问方式来操作一个包含指向 ThreadPoolExecutor 的指针的向量
    using SyncVecThreadPoolExecutors =
            folly::Synchronized<std::vector<ThreadPoolExecutor *>>;

    // 提供了一个全局的、线程安全的对 SyncVecThreadPoolExecutors 的引用
    // 这种设计模式常用于单例或全局资源的线程安全管理
    SyncVecThreadPoolExecutors &getSyncVecThreadPoolExecutors() {
        // Indestructible 是一个模板类
        // 通常用于确保某些静态对象不会在程序退出时被析构，从而避免程序退出时的某些问题
        static folly::Indestructible<SyncVecThreadPoolExecutors> storage;
        return *storage;
    }

    void ThreadPoolExecutor::registerThreadPoolExecutor(ThreadPoolExecutor *tpe) {
        getSyncVecThreadPoolExecutors().wlock()->push_back(tpe);
    }

    void ThreadPoolExecutor::deregisterThreadPoolExecutor(ThreadPoolExecutor *tpe) {
        getSyncVecThreadPoolExecutors().withWLock([tpe](auto &tpes) {
            tpes.erase(std::remove(tpes.begin(), tpes.end(), tpe), tpes.end());
        });
    }

    FOLLY_GFLAGS_DEFINE_int64(
            threadtimeout_ms,
            60000,
            "Idle time before ThreadPoolExecutor threads are joined");

    ThreadPoolExecutor::ThreadPoolExecutor(
            size_t /* maxThreads */,
            size_t minThreads,
            std::shared_ptr<ThreadFactory> threadFactory)
            : threadFactory_(std::move(threadFactory)),
              taskStatsCallbacks_(std::make_shared<TaskStatsCallbackRegistry>()),
              threadPoolHook_("folly::ThreadPoolExecutor"),
              minThreads_(minThreads) {
        threadTimeout_ = std::chrono::milliseconds(FLAGS_threadtimeout_ms);
    }

    ThreadPoolExecutor::~ThreadPoolExecutor() {
        joinKeepAliveOnce();
        CHECK_EQ(0, threadList_.get().size());
    }

    ThreadPoolExecutor::Task::Task(
            Func &&func, std::chrono::milliseconds expiration, Func &&expireCallback)
            : func_(std::move(func)),
            // Assume that the task in enqueued on creation
              enqueueTime_(std::chrono::steady_clock::now()),
              context_(folly::RequestContext::saveContext()) {
        if (expiration > std::chrono::milliseconds::zero()) {
            expiration_ = std::make_unique<Expiration>();
            expiration_->expiration = expiration;
            expiration_->expireCallback = std::move(expireCallback);
        }
    }

    void ThreadPoolExecutor::runTask(const ThreadPtr &thread, Task &&task) {

        // 原子操作将线程的空闲状态设置为 false
        thread->idle.store(false, std::memory_order_relaxed);

        // 记录任务开始执行的时间
        auto startTime = std::chrono::steady_clock::now();

        // 初始化任务统计信息
        TaskStats stats;
        stats.enqueueTime = task.enqueueTime_;
        // 如果任务有关联的上下文，那么 stats.requestId 记录的是这个上下文的根 ID
        if (task.context_) {
            stats.requestId = task.context_->getRootId();
        }
        // 任务从进入队列到开始执行所等待的时间
        stats.waitTime = startTime - task.enqueueTime_;

        {
            // 使用了 RequestContextScopeGuard 来为任务设置一个特定的上下文，
            // 确保任务在这个上下文中执行
            folly::RequestContextScopeGuard rctx(task.context_);

            // 任务过期处理的逻辑

            // 检查任务是否已过期
            if (task.expiration_ != nullptr &&
                stats.waitTime >= task.expiration_->expiration) {
                // 任务的函数被设置为 nullptr，意味着这个任务不会被执行
                task.func_ = nullptr;
                // 将统计数据中的“过期”标记设置为 true
                stats.expired = true;
                // 如果任务过期并且设置了一个到期回调，那么这个回调会被执行
                if (task.expiration_->expireCallback != nullptr) {
                    invokeCatchingExns(
                            "ThreadPoolExecutor: expireCallback",
                            std::exchange(task.expiration_->expireCallback, {}));
                }
                // 如果任务没有过期
            } else {
                // 执行任务，调用并捕获异常
                // 执行 task.func_ 这个函数，并立即将其替换为一个空的初始化器。
                // 这样做的目的是为了确保任务函数只执行一次，执行完之后就被移除或置空
                invokeCatchingExns(
                        "ThreadPoolExecutor: func", std::exchange(task.func_, {}));
                // 清除过期回调
                if (task.expiration_ != nullptr) {
                    task.expiration_->expireCallback = nullptr;
                }
            }
        }
        // 计算并记录那些未过期任务的执行运行时间
        if (!stats.expired) {
            stats.runTime = std::chrono::steady_clock::now() - startTime;
        }

        // Times in this USDT use granularity of std::chrono::steady_clock::duration,
        // which is platform dependent. On Facebook servers, the granularity is
        // nanoseconds. We explicitly do not perform any unit conversions to avoid
        // unnecessary costs and leave it to consumers of this data to know what
        // effective clock resolution is.
        FOLLY_SDT(
                folly,
                thread_pool_executor_task_stats,
                threadFactory_->getNamePrefix().c_str(),
                stats.requestId,
                stats.enqueueTime.time_since_epoch().count(),
                stats.waitTime.count(),
                stats.runTime.count());

        // 设置线程为 idle 状态
        thread->idle.store(true, std::memory_order_relaxed);

        // 更新线程的最后活跃时间
        thread->lastActiveTime.store(
                std::chrono::steady_clock::now(), std::memory_order_relaxed);

        // 处理任务统计的回调
        thread->taskStatsCallbacks->callbackList.withRLock([&](auto &callbacks) {
            *thread->taskStatsCallbacks->inCallback = true;
            SCOPE_EXIT { *thread->taskStatsCallbacks->inCallback = false; };
            invokeCatchingExns("ThreadPoolExecutor: task stats callback", [&] {
                for (auto &callback: callbacks) {
                    callback(stats);
                }
            });
        });
    }

    void ThreadPoolExecutor::add(Func, std::chrono::milliseconds, Func) {
        throw std::runtime_error(
                "add() with expiration is not implemented for this Executor");
    }

    size_t ThreadPoolExecutor::numThreads() const {
        return maxThreads_.load(std::memory_order_relaxed);
    }

    size_t ThreadPoolExecutor::numActiveThreads() const {
        return activeThreads_.load(std::memory_order_relaxed);
    }

// Set the maximum number of running threads.
    // 动态地设置线程池中的线程数量
    void ThreadPoolExecutor::setNumThreads(size_t numThreads) {
        /* Since ThreadPoolExecutor may be dynamically adjusting the number of
           threads, we adjust the relevant variables instead of changing
           the number of threads directly.  Roughly:

           If numThreads < minthreads reset minThreads to numThreads.

           If numThreads < active threads, reduce number of running threads.

           If the number of pending tasks is > 0, then increase the currently
           active number of threads such that we can run all the tasks, or reach
           numThreads.

           Note that if there are observers, we actually have to create all
           the threads, because some observer implementations need to 'observe'
           all thread creation (see tests for an example of this)
        */

        // 记录需要被终止并加入的线程数量
        size_t numThreadsToJoin = 0;
        {
            // 用写锁保护了下面的代码块，确保线程安全
            folly::SharedMutex::WriteHolder w{&threadListLock_};

            // 获取等待队列中待执行任务的数量
            auto pending = getPendingTaskCountImpl();
            maxThreads_.store(numThreads, std::memory_order_relaxed);

            // 加载当前活动线程数和最小线程数
            auto active = activeThreads_.load(std::memory_order_relaxed);
            auto minthreads = minThreads_.load(std::memory_order_relaxed);

            // 如果要设置的线程数小于最小线程数，则将最小线程数设置为 numThreads
            if (numThreads < minthreads) {
                minthreads = numThreads;
                minThreads_.store(numThreads, std::memory_order_relaxed);
            }

            // 如果当前活动的线程数大于要设置的线程数，计算差值并删除多余的线程。
            if (active > numThreads) {
                numThreadsToJoin = active - numThreads;
                assert(numThreadsToJoin <= active - minthreads);
                ThreadPoolExecutor::removeThreads(numThreadsToJoin, false);
                activeThreads_.store(numThreads, std::memory_order_relaxed);

                // 如果有待执行的任务、存在观察者或活动线程数小于最小线程数，根据需要增加线程。
            } else if (pending > 0 || !observers_.empty() || active < minthreads) {
                size_t numToAdd = std::min(pending, numThreads - active);
                if (!observers_.empty()) {
                    numToAdd = numThreads - active;
                }
                if (active + numToAdd < minthreads) {
                    numToAdd = minthreads - active;
                }
                ThreadPoolExecutor::addThreads(numToAdd);
                activeThreads_.store(active + numToAdd, std::memory_order_relaxed);
            }
            // 释放写锁。
        }

        /* We may have removed some threads, attempt to join them */
        // 尝试加入已停止的线程。
        joinStoppedThreads(numThreadsToJoin);
    }

// threadListLock_ is writelocked
    // 动态地向线程池中添加指定数量的线程
    // 并确保这些线程正确启动
    // 如果有观察者，它们会被通知新线程的启动
    void ThreadPoolExecutor::addThreads(size_t n) {
        // 创建一个 newThreads 的线程指针容器，用于存放新建的线程
        std::vector<ThreadPtr> newThreads;
        for (size_t i = 0; i < n; i++) {
            // 每次调用 makeThread() 方法来创建一个新线程
            newThreads.push_back(makeThread());
        }

        // 对 newThreads 容器中的每个线程进行操作
        for (auto &thread: newThreads) {
            // TODO need a notion of failing to create the thread
            // and then handling for that case
            // 通过 threadFactory_ 的 newThread 方法创建一个新的线程，
            // 并与 ThreadPoolExecutor::threadRun 方法绑定
            thread->handle = threadFactory_->newThread(
                    std::bind(&ThreadPoolExecutor::threadRun, this, thread));

            // 将新创建的线程添加到 threadList_ 中
            threadList_.add(thread);
        }

        // 再次遍历 newThreads 容器中的每个线程，等待每个线程启动完成
        // 使用 Baton 进行同步，确保线程启动完成
        for (auto &thread: newThreads) {
            thread->startupBaton.wait(
                    Baton<>::wait_options().logging_enabled(false));
        }

        // 遍历所有的观察者(observers_)，并对每个新线程调用其 threadStarted 方法。
        // 这可能是为了让观察者知道有新线程启动
        for (auto &o: observers_) {
            for (auto &thread: newThreads) {
                o->threadStarted(thread.get());
            }
        }
    }

// threadListLock_ is writelocked
    // isJoin 是否需要在移除线程后进行 join 操作
    void ThreadPoolExecutor::removeThreads(size_t n, bool isJoin) {
        isJoin_ = isJoin;
        stopThreads(n);
    }

    void ThreadPoolExecutor::joinStoppedThreads(size_t n) {
        for (size_t i = 0; i < n; i++) {
            auto thread = stoppedThreads_.take();
            thread->handle.join();
        }
    }

    // 停止线程池
    void ThreadPoolExecutor::stop() {
        // 释放 KeepAlive 的资源
        joinKeepAliveOnce();

        // 需要 join 的线程数量
        size_t n = 0;
        {
            // 获取 threadListLock_ 的写锁
            folly::SharedMutex::WriteHolder w{&threadListLock_};

            // 将 maxThreads_ 和 activeThreads_ 设置为0，
            // 这意味着线程池中不再允许有线程
            maxThreads_.store(0, std::memory_order_release);
            activeThreads_.store(0, std::memory_order_release);

            // 获取当前活跃线程的数量，并赋值给 n
            n = threadList_.get().size();

            // 移除所有活跃线程
            removeThreads(n, false);

            // 加载 threadsToJoin_ 的值（表示需要加入的线程数量），
            // 并与当前n的值相加
            n += threadsToJoin_.load(std::memory_order_relaxed);
            threadsToJoin_.store(0, std::memory_order_relaxed);

            // 释放写锁。
        }

        // join 已停止的线程，确保所有线程都已完全执行完毕
        joinStoppedThreads(n);

        // 检查 threadList_ 和 stoppedThreads_ 的大小是否都为0，
        // 确保所有线程都已被正确处理
        CHECK_EQ(0, threadList_.get().size());
        CHECK_EQ(0, stoppedThreads_.size());
    }

    // 停止并 join 线程池中的所有线程
    // 这与之前的 stop 方法非常相似，
    // 但关键的区别在于 removeThreads 函数调用中的 isJoin 参数被设置为 true，
    // 这表示我们的目的是 join 线程，而不仅仅是停止它们
    void ThreadPoolExecutor::join() {
        joinKeepAliveOnce();
        size_t n = 0;
        {
            folly::SharedMutex::WriteHolder w{&threadListLock_};
            maxThreads_.store(0, std::memory_order_release);
            activeThreads_.store(0, std::memory_order_release);
            n = threadList_.get().size();
            removeThreads(n, true);
            n += threadsToJoin_.load(std::memory_order_relaxed);
            threadsToJoin_.store(0, std::memory_order_relaxed);
        }
        joinStoppedThreads(n);
        CHECK_EQ(0, threadList_.get().size());
        CHECK_EQ(0, stoppedThreads_.size());
    }

    void ThreadPoolExecutor::withAll(folly::FunctionRef<void(ThreadPoolExecutor &)> f) {
        // getSyncVecThreadPoolExecutors 函数返回一个包含所有线程池执行器的同步向量）
        // 获取一个读锁并执行给定的 lambda 函数，
        // 其中lambda函数接受 tpes（线程池执行器的集合）作为参数
        getSyncVecThreadPoolExecutors().withRLock([f](auto &tpes) {
            for (auto tpe: tpes) {
                f(*tpe);
            }
        });
    }

    // 获取线程池的统计信息
    // 遍历线程池中的所有线程，统计空闲和活动线程的数量，
    // 然后计算其他相关的统计数据，并返回这些数据
    ThreadPoolExecutor::PoolStats ThreadPoolExecutor::getPoolStats() const {
        const auto now = std::chrono::steady_clock::now();
        folly::SharedMutex::ReadHolder r{&threadListLock_};
        ThreadPoolExecutor::PoolStats stats;
        size_t activeTasks = 0;
        size_t idleAlive = 0;
        for (const auto &thread: threadList_.get()) {
            if (thread->idle.load(std::memory_order_relaxed)) {
                const std::chrono::nanoseconds idleTime =
                        now - thread->lastActiveTime.load(std::memory_order_relaxed);
                stats.maxIdleTime = std::max(stats.maxIdleTime, idleTime);
                idleAlive++;
            } else {
                activeTasks++;
            }
        }
        stats.pendingTaskCount = getPendingTaskCountImpl();
        stats.totalTaskCount = stats.pendingTaskCount + activeTasks;

        stats.threadCount = maxThreads_.load(std::memory_order_relaxed);
        stats.activeThreadCount =
                activeThreads_.load(std::memory_order_relaxed) - idleAlive;
        stats.idleThreadCount = stats.threadCount - stats.activeThreadCount;
        return stats;
    }

    // 获取线程池中待处理的任务数量
    // 在一个读锁的保护下，安全地获取
    size_t ThreadPoolExecutor::getPendingTaskCount() const {
        folly::SharedMutex::ReadHolder r{&threadListLock_};
        return getPendingTaskCountImpl();
    }

    const std::string &ThreadPoolExecutor::getName() const {
        return threadFactory_->getNamePrefix();
    }

    std::atomic<uint64_t> ThreadPoolExecutor::Thread::nextId(0);

    std::chrono::nanoseconds ThreadPoolExecutor::Thread::usedCpuTime() const {
        using std::chrono::nanoseconds;
        using std::chrono::seconds;
        timespec tp{};
#ifdef __linux__
        clockid_t clockid;
        auto th = const_cast<std::thread&>(handle).native_handle();
        if (!pthread_getcpuclockid(th, &clockid)) {
          clock_gettime(clockid, &tp);
        }
#endif
        return nanoseconds(tp.tv_nsec) + seconds(tp.tv_sec);
    }

    // 订阅任务的统计信息。
    // 当有任务的统计信息更新时，用户提供的回调函数会被调用。
    // 但是，为了确保线程安全性，该函数不允许在任务统计的回调中再次订阅
    void ThreadPoolExecutor::subscribeToTaskStats(TaskStatsCallback cb) {
        if (*taskStatsCallbacks_->inCallback) {
            throw std::runtime_error("cannot subscribe in task stats callback");
        }
        taskStatsCallbacks_->callbackList.wlock()->push_back(std::move(cb));
    }

    // 向队列中添加一个线程，并通过信号量进行通知其他可能在等待的线程
    BlockingQueueAddResult ThreadPoolExecutor::StoppedThreadQueue::add(
            ThreadPoolExecutor::ThreadPtr item) {
        std::lock_guard<std::mutex> guard(mutex_);
        queue_.push(std::move(item));
        return sem_.post();
    }

    // 从队列中取出一个线程，如果队列是空的，它会等待直到有线程可供取出
    ThreadPoolExecutor::ThreadPtr ThreadPoolExecutor::StoppedThreadQueue::take() {
        while (true) {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (!queue_.empty()) {
                    auto item = std::move(queue_.front());
                    queue_.pop();
                    return item;
                }
            }
            sem_.wait();
        }
    }

    folly::Optional<ThreadPoolExecutor::ThreadPtr>
    ThreadPoolExecutor::StoppedThreadQueue::try_take_for(
            std::chrono::milliseconds time) {
        while (true) {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (!queue_.empty()) {
                    auto item = std::move(queue_.front());
                    queue_.pop();
                    return item;
                }
            }
            if (!sem_.try_wait_for(time)) {
                return folly::none;
            }
        }
    }

    size_t ThreadPoolExecutor::StoppedThreadQueue::size() {
        std::lock_guard<std::mutex> guard(mutex_);
        return queue_.size();
    }

    // 将一个观察者添加到线程池中，并为此观察者通知所有已经启动过的线程
    void ThreadPoolExecutor::addObserver(std::shared_ptr<Observer> o) {
        {
            folly::SharedMutex::WriteHolder r{&threadListLock_};
            observers_.push_back(o);
            for (auto &thread: threadList_.get()) {
                o->threadPreviouslyStarted(thread.get());
            }
        }
        ensureMaxActiveThreads();
    }

    // 从线程池的观察者列表中删除一个指定的观察者，并通知这个观察者线程池中所有还未停止的线程
    void ThreadPoolExecutor::removeObserver(std::shared_ptr<Observer> o) {
        folly::SharedMutex::WriteHolder r{&threadListLock_};
        for (auto &thread: threadList_.get()) {
            o->threadNotYetStopped(thread.get());
        }

        for (auto it = observers_.begin(); it != observers_.end(); it++) {
            if (*it == o) {
                observers_.erase(it);
                return;
            }
        }
        DCHECK(false);
    }

// Idle threads may have destroyed themselves, attempt to join
// them here
    // 确保已经被标记为需要 join 的线程被合适地 join
    void ThreadPoolExecutor::ensureJoined() {
        auto tojoin = threadsToJoin_.load(std::memory_order_relaxed);
        if (tojoin) {
            {
                folly::SharedMutex::WriteHolder w{&threadListLock_};
                tojoin = threadsToJoin_.load(std::memory_order_relaxed);
                threadsToJoin_.store(0, std::memory_order_relaxed);
            }
            joinStoppedThreads(tojoin);
        }
    }

// threadListLock_ must be write locked.
    // 尝试因超时而停止一个线程
    bool ThreadPoolExecutor::tryTimeoutThread() {
        // Try to stop based on idle thread timeout (try_take_for),
        // if there are at least minThreads running.
        if (!minActive()) {
            return false;
        }

        // Remove thread from active count
        activeThreads_.store(
                activeThreads_.load(std::memory_order_relaxed) - 1,
                std::memory_order_relaxed);

        // There is a memory ordering constraint w.r.t the queue
        // implementation's add() and getPendingTaskCountImpl() - while many
        // queues have seq_cst ordering, some do not, so add an explicit
        // barrier.  tryTimeoutThread is the slow path and only happens once
        // every thread timeout; use asymmetric barrier to keep add() fast.
        folly::asymmetric_thread_fence_heavy(std::memory_order_seq_cst);

        // If this is based on idle thread timeout, then
        // adjust vars appropriately (otherwise stop() or join()
        // does this).
        if (getPendingTaskCountImpl() > 0) {
            // There are still pending tasks, we can't stop yet.
            // re-up active threads and return.
            activeThreads_.store(
                    activeThreads_.load(std::memory_order_relaxed) + 1,
                    std::memory_order_relaxed);
            return false;
        }

        threadsToJoin_.store(
                threadsToJoin_.load(std::memory_order_relaxed) + 1,
                std::memory_order_relaxed);

        return true;
    }

// If we can't ensure that we were able to hand off a task to a thread,
// attempt to start a thread that handled the task, if we aren't already
// running the maximum number of threads.
    // 确保至少有一个线程是活跃的，如果没有，就启动一个
    void ThreadPoolExecutor::ensureActiveThreads() {
        ensureJoined();

        // Matches barrier in tryTimeoutThread().  Ensure task added
        // is seen before loading activeThreads_ below.
        folly::asymmetric_thread_fence_light(std::memory_order_seq_cst);

        // Fast path assuming we are already at max threads.
        auto active = activeThreads_.load(std::memory_order_relaxed);
        auto total = maxThreads_.load(std::memory_order_relaxed);

        if (active >= total) {
            return;
        }

        folly::SharedMutex::WriteHolder w{&threadListLock_};
        // Double check behind lock.
        active = activeThreads_.load(std::memory_order_relaxed);
        total = maxThreads_.load(std::memory_order_relaxed);
        if (active >= total) {
            return;
        }
        ThreadPoolExecutor::addThreads(1);
        activeThreads_.store(active + 1, std::memory_order_relaxed);
    }

    // 确保活动线程的数量等于最大线程数。
    // 如果活动线程的数量小于最大线程数，则调用 ensureActiveThreads()
    void ThreadPoolExecutor::ensureMaxActiveThreads() {
        while (activeThreads_.load(std::memory_order_relaxed) <
               maxThreads_.load(std::memory_order_relaxed)) {
            ensureActiveThreads();
        }
    }

// If an idle thread times out, only join it if there are at least
// minThreads threads.
    // 判断当前活跃的线程数量是否超过了最小线程数
    bool ThreadPoolExecutor::minActive() {
        return activeThreads_.load(std::memory_order_relaxed) >
               minThreads_.load(std::memory_order_relaxed);
    }

} // namespace ThreadPool

