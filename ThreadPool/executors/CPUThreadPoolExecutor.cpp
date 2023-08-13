#include "Executor.h"
#include "executors/CPUThreadPoolExecutor.h"

#include <atomic>
#include <folly/Memory.h>
#include <folly/Optional.h>
#include "executors/QueueObserver.h"
#include "executors/task_queue/PriorityLifoSemMPMCQueue.h"
#include "executors/task_queue/PriorityUnboundedBlockingQueue.h"
#include "executors/task_queue/UnboundedBlockingQueue.h"
#include <folly/portability/GFlags.h>
#include <folly/synchronization/ThrottledLifoSem.h>

FOLLY_GFLAGS_DEFINE_bool(
        dynamic_cputhreadpoolexecutor,
        true,
        "CPUThreadPoolExecutor will dynamically create and destroy threads");

namespace ThreadPool {

    const size_t CPUThreadPoolExecutor::kDefaultMaxQueueSize = 1 << 14;

/* static */ auto CPUThreadPoolExecutor::makeDefaultQueue()
    -> std::unique_ptr<BlockingQueue<CPUTask>> {
        return std::make_unique<UnboundedBlockingQueue<CPUTask>>();
    }

/* static */ auto CPUThreadPoolExecutor::makeDefaultPriorityQueue(
            int8_t numPriorities) -> std::unique_ptr<BlockingQueue<CPUTask>> {
        CHECK_GT(numPriorities, 0) << "Number of priorities should be positive";
        return std::make_unique<PriorityUnboundedBlockingQueue<CPUTask>>(
                numPriorities);
    }

/* static */ auto CPUThreadPoolExecutor::makeThrottledLifoSemQueue(
            std::chrono::nanoseconds wakeUpInterval)
    -> std::unique_ptr<BlockingQueue<CPUTask>> {
        folly::ThrottledLifoSem::Options opts;
        opts.wakeUpInterval = wakeUpInterval;
        return std::make_unique<UnboundedBlockingQueue<CPUTask, folly::ThrottledLifoSem>>(
                opts);
    }

/* static */ auto CPUThreadPoolExecutor::makeThrottledLifoSemPriorityQueue(
            int8_t numPriorities, std::chrono::nanoseconds wakeUpInterval)
    -> std::unique_ptr<BlockingQueue<CPUTask>> {
        folly::ThrottledLifoSem::Options opts;
        opts.wakeUpInterval = wakeUpInterval;
        return std::make_unique<
                PriorityUnboundedBlockingQueue<CPUTask, folly::ThrottledLifoSem>>(
                numPriorities, opts);
    }

    CPUThreadPoolExecutor::CPUThreadPoolExecutor(
            size_t numThreads,
            std::unique_ptr<BlockingQueue<CPUTask>> taskQueue,
            std::shared_ptr<ThreadFactory> threadFactory,
            Options opt)
            : CPUThreadPoolExecutor(
            std::make_pair(
                    numThreads, FLAGS_dynamic_cputhreadpoolexecutor ? 0 : numThreads),
            std::move(taskQueue),
            std::move(threadFactory),
            std::move(opt)) {}

    CPUThreadPoolExecutor::CPUThreadPoolExecutor(
            std::pair<size_t, size_t> numThreads,
            std::unique_ptr<BlockingQueue<CPUTask>> taskQueue,
            std::shared_ptr<ThreadFactory> threadFactory,
            Options opt)
            : ThreadPoolExecutor(
            numThreads.first, numThreads.second, std::move(threadFactory)),
              taskQueue_(taskQueue.release()),
              queueObserverFactory_{
                      !opt.queueObserverFactory ? createQueueObserverFactory()
                                                : std::move(opt.queueObserverFactory)},
              prohibitBlockingOnThreadPools_{opt.blocking} {
        setNumThreads(numThreads.first);
        if (numThreads.second == 0) {
            minThreads_.store(1, std::memory_order_relaxed);
        }
        registerThreadPoolExecutor(this);
    }

    CPUThreadPoolExecutor::CPUThreadPoolExecutor(
            size_t numThreads,
            std::shared_ptr<ThreadFactory> threadFactory,
            Options opt)
            : CPUThreadPoolExecutor(
            std::make_pair(
                    numThreads, FLAGS_dynamic_cputhreadpoolexecutor ? 0 : numThreads),
            std::move(threadFactory),
            std::move(opt)) {}

    CPUThreadPoolExecutor::CPUThreadPoolExecutor(
            std::pair<size_t, size_t> numThreads,
            std::shared_ptr<ThreadFactory> threadFactory,
            Options opt)
            : ThreadPoolExecutor(
            numThreads.first, numThreads.second, std::move(threadFactory)),
              taskQueue_(makeDefaultQueue()),
              queueObserverFactory_{
                      opt.queueObserverFactory ? std::move(opt.queueObserverFactory)
                                               : createQueueObserverFactory()},
              prohibitBlockingOnThreadPools_{opt.blocking} {
        setNumThreads(numThreads.first);
        if (numThreads.second == 0) {
            minThreads_.store(1, std::memory_order_relaxed);
        }
        registerThreadPoolExecutor(this);
    }

    CPUThreadPoolExecutor::CPUThreadPoolExecutor(size_t numThreads, Options opt)
            : CPUThreadPoolExecutor(
            numThreads,
            std::make_shared<NamedThreadFactory>("CPUThreadPool"),
            std::move(opt)) {}

    CPUThreadPoolExecutor::CPUThreadPoolExecutor(
            size_t numThreads,
            int8_t numPriorities,
            std::shared_ptr<ThreadFactory> threadFactory,
            Options opt)
            : CPUThreadPoolExecutor(
            numThreads,
            makeDefaultPriorityQueue(numPriorities),
            std::move(threadFactory),
            std::move(opt)) {}

    CPUThreadPoolExecutor::CPUThreadPoolExecutor(
            size_t numThreads,
            int8_t numPriorities,
            size_t maxQueueSize,
            std::shared_ptr<ThreadFactory> threadFactory,
            Options opt)
            : CPUThreadPoolExecutor(
            numThreads,
            std::make_unique<PriorityLifoSemMPMCQueue<CPUTask>>(
                    numPriorities, maxQueueSize),
            std::move(threadFactory),
            std::move(opt)) {}

    CPUThreadPoolExecutor::~CPUThreadPoolExecutor() {
        deregisterThreadPoolExecutor(this);
        stop();
        CHECK(threadsToStop_ == 0);
        if (getNumPriorities() == 1) {
            delete queueObservers_[0];
        } else {
            for (auto &observer: queueObservers_) {
                delete observer.load(std::memory_order_relaxed);
            }
        }
    }

    // 获取指定优先级任务队列的 QueueObserver 对象
    QueueObserver *FOLLY_NULLABLE
    CPUThreadPoolExecutor::getQueueObserver(int8_t pri) {
        // 检查 queueObserverFactory_ 是否存在,如果不存在则返回 nullptr
        if (!queueObserverFactory_) {
            return nullptr;
        }
        // 从 queueObservers_ 数组中获取指定优先级对应的 observer 指针
        auto &slot = queueObservers_[folly::to_unsigned(pri)];

        // 如果该 observer 已存在,直接返回
        if (auto observer = slot.load(std::memory_order_acquire)) {
            return observer;
        }

        // common case is only one queue, need only one observer
        // 如果只有一个优先级(默认情况),则共享使用 prio 0 的 observer
        if (getNumPriorities() == 1 && pri != 0) {
            auto sharedObserver = getQueueObserver(0);
            slot.store(sharedObserver, std::memory_order_release);
            return sharedObserver;
        }

        // 否则调用 queueObserverFactory_ 的 create() 方法创建新的 observer
        QueueObserver *existingObserver = nullptr;
        auto newObserver = queueObserverFactory_->create(pri);

        // 使用 CAS 操作尝试设置新 observer,避免重复创建
        if (!slot.compare_exchange_strong(existingObserver, newObserver.get())) {
            return existingObserver;
        } else {
            // 如果 CAS 失败则返回已存在的 observer,成功则返回新创建的
            return newObserver.release();
        }

        // 这样通过懒加载避免不必要的 observer 创建,同时处理了单/多优先级情况下的对应逻辑
    }

    void CPUThreadPoolExecutor::add(Func func) {
        add(std::move(func), std::chrono::milliseconds(0));
    }

    void CPUThreadPoolExecutor::add(
            Func func, std::chrono::milliseconds expiration, Func expireCallback) {
        addImpl<false>(std::move(func), 0, expiration, std::move(expireCallback));
    }

    void CPUThreadPoolExecutor::addWithPriority(Func func, int8_t priority) {
        add(std::move(func), priority, std::chrono::milliseconds(0));
    }

    void CPUThreadPoolExecutor::add(
            Func func,
            int8_t priority,
            std::chrono::milliseconds expiration,
            Func expireCallback) {
        // 内部调用的是 addImpl() 模板函数,真正完成添加任务的逻辑
        // addImpl() 会把 func 封装成一个 CPUTask 对象,该对象持有 func、priority、expiration 等信息。
        // 然后根据线程池的任务队列类型,将 CPUTask 提交给阻塞队列:
        // 1. 如果是普通队列,则直接添加任务
        // 2. 如果是优先级队列,则根据 priority 插入到合适的优先级子队列
        // 3. 如果任务已过期,则直接调用 expireCallback
        // 最后唤醒等待线程来开始执行任务。
        addImpl<true>(
                std::move(func), priority, expiration, std::move(expireCallback));
    }

    template<bool withPriority>
    void CPUThreadPoolExecutor::addImpl(
            Func func,
            int8_t priority,
            std::chrono::milliseconds expiration,
            Func expireCallback) {

        // 根据 withPriority 模板参数判断是否需要处理优先级
        if (withPriority) {
            CHECK(getNumPriorities() > 0);
        }

        // 用参数构造一个 CPUTask 对象,包装了任务函数、优先级等信息
        CPUTask task(
                std::move(func), expiration, std::move(expireCallback), priority);

        // 如果设置了队列观察器,调用其 onEnqueued() 回调
        if (auto queueObserver = getQueueObserver(priority)) {
            task.queueObserverPayload() =
                    queueObserver->onEnqueued(task.context_.get());

        }

        // It's not safe to expect that the executor is alive after a task is added to
        // the queue (this task could be holding the last KeepAlive and when finished
        // - it may unblock the executor shutdown).
        // If we need executor to be alive after adding into the queue, we have to
        // acquire a KeepAlive.

        // 获取一个 KeepAlive 句柄,确保添加任务后线程池不会被关闭
        bool mayNeedToAddThreads = minThreads_.load(std::memory_order_relaxed) == 0 ||
                                   activeThreads_.load(std::memory_order_relaxed) <
                                   maxThreads_.load(std::memory_order_relaxed);
        ThreadPool::Executor::KeepAlive<> ka = mayNeedToAddThreads
                                          ? getKeepAliveToken(this)
                                          : ThreadPool::Executor::KeepAlive<>{};

        // 根据优先级,决定调用 taskQueue 的哪个添加任务方法
        auto result = withPriority
                      ? taskQueue_->addWithPriority(std::move(task), priority)
                      : taskQueue_->add(std::move(task));

        // 如果需要(最小线程数为 0 或活跃线程数少于最大数),
        // 调用 ensureActiveThreads() 确保创建足够线程
        if (mayNeedToAddThreads && !result.reusedThread) {
            ensureActiveThreads();
        }
    }

    uint8_t CPUThreadPoolExecutor::getNumPriorities() const {
        return taskQueue_->getNumPriorities();
    }

    size_t CPUThreadPoolExecutor::getTaskQueueSize() const {
        return taskQueue_->size();
    }

    BlockingQueue<CPUThreadPoolExecutor::CPUTask> *
    CPUThreadPoolExecutor::getTaskQueue() {
        return taskQueue_.get();
    }

// threadListLock_ must be writelocked.
    // 是否正在尝试减少需要停止的线程数
    // 这个方法需要修改 threadsToStop_,所以调用时需要先获取 threadListLock_ 的写锁
    bool CPUThreadPoolExecutor::tryDecrToStop() {
        // 加载 threadsToStop_ 的当前值到 toStop 变量
        auto toStop = threadsToStop_.load(std::memory_order_relaxed);

        // 判断 toStop 是否大于0,如果不大于 0 则返回 false,表示不需要停止线程
        if (toStop <= 0) {
            return false;
        }

        // 如果 toStop 大于 0,则将 threadsToStop_ 减 1,并返回 true,表示正在停止一个线程
        threadsToStop_.store(toStop - 1, std::memory_order_relaxed);
        return true;
    }

    // 是否应该停止当前工作线程
    bool CPUThreadPoolExecutor::taskShouldStop(folly::Optional<CPUTask> &task) {
        if (tryDecrToStop()) {
            return true;
        }

        // task 是否取到了有效任务,如果取到任务则返回 false,表示不应停止
        if (task) {
            return false;
        } else {
            // 如果 task 为空,则调用 tryTimeoutThread() 检查是否发生了线程空闲超时,
            // 如果超时了也应该停止线程,则返回 true
            return tryTimeoutThread();
        }
        return true;
    }

    // 每个工作线程执行的函数
    void CPUThreadPoolExecutor::threadRun(ThreadPtr thread) {
        // 注册线程 ID
        this->threadPoolHook_.registerThread();

        // 根据 options 设置 ExecutorBlockingGuard，用于控制和监控在线程池线程上发生阻塞调用的类
        // 如果设置了 Options::prohibitBlockingOnThreadPools,则使用 ProhibitTag 创建 ExecutorBlockingGuard,禁止在线程池线程上阻塞
        // 否则使用 TrackTag 创建 ExecutorBlockingGuard,追踪在线程池线程上发生的阻塞调用
        folly::Optional<ExecutorBlockingGuard> guard; // optional until C++17
        if (prohibitBlockingOnThreadPools_ == Options::Blocking::prohibit) {
            guard.emplace(ExecutorBlockingGuard::ProhibitTag{}, this, getName());
        } else {
            guard.emplace(ExecutorBlockingGuard::TrackTag{}, this, getName());
        }

        // 通知 startupBaton 线程启动完成
        thread->startupBaton.post();

        // 在 threadIdCollector 加入线程ID
        threadIdCollector_->addTid(folly::getOSThreadID());

        // On thread exit, we should remove the thread ID from the tracking list.
        // 在作用域退出时自动调用 threadIdCollector_->removeTid() 取消注册
        auto threadIDsGuard = folly::makeGuard([this]() {
            // The observer could be capturing a stack trace from this thread
            // so it should block until the collection finishes to exit.
            threadIdCollector_->removeTid(folly::getOSThreadID());
        });

        // 循环获取任务并执行
        while (true) {
            // 取任务
            auto task = taskQueue_->try_take_for(
                    threadTimeout_.load(std::memory_order_relaxed));

            // Handle thread stopping, either by task timeout, or
            // by 'poison' task added in join() or stop().
            // 如果取到的是超时或毒性任务,结束线程执行
            if (FOLLY_UNLIKELY(!task || task.value().poison)) {
                // Actually remove the thread from the list.
                folly::SharedMutex::WriteHolder w{&threadListLock_};
                if (taskShouldStop(task)) {
                    for (auto &o: observers_) {
                        o->threadStopped(thread.get());
                    }
                    threadList_.remove(thread);
                    stoppedThreads_.add(thread);
                    return;
                } else {
                    continue;
                }
            }

            // 如果有队列观察器,调用其 onDequeued()
            if (auto queueObserver = getQueueObserver(task->queuePriority())) {
                queueObserver->onDequeued(task->queueObserverPayload());
            }

            // 通过 runTask() 执行任务
            runTask(thread, std::move(task.value()));

            // 如果设置了要停止的线程数,尝试使用 tryDecrToStop() 停止当前线程
            // 删除线程引用,通知观察器线程已停止
            if (FOLLY_UNLIKELY(threadsToStop_ > 0 && !isJoin_)) {
                folly::SharedMutex::WriteHolder w{&threadListLock_};
                if (tryDecrToStop()) {
                    threadList_.remove(thread);
                    stoppedThreads_.add(thread);
                    return;
                }
            }
        }
    }

    // 停止线程池中的 n 个线程
    void CPUThreadPoolExecutor::stopThreads(size_t n) {
        // 首先将 threadsToStop_ 加 n,记录需要停止的线程数
        threadsToStop_ += n;

        // 然后循环 n 次,每次向任务队列添加一个空任务(CPUTask()),
        // 优先级为最低(Executor::LO_PRI)
        // 添加空任务的目的是为了在工作线程的任务循环中停止线程的执行。
        //工作线程在获取任务时,会优先获取高优先级任务。
        // 当队列中有 n 个最低级的空任务时,会有 n 个工作线程获取到这些空任务,从而结束它们的执行循环
        for (size_t i = 0; i < n; i++) {
            taskQueue_->addWithPriority(CPUTask(), Executor::LO_PRI);
        }
    }

// threadListLock_ is read (or write) locked.
    size_t CPUThreadPoolExecutor::getPendingTaskCountImpl() const {
        return taskQueue_->size();
    }

    // 创建 QueueObserverFactory 对象
    std::unique_ptr<ThreadPool::QueueObserverFactory>
    CPUThreadPoolExecutor::createQueueObserverFactory() {
        // 首先对 queueObservers_ 数组中的 observer 指针都设置为 nullptr。
        // 这是为了释放之前可能存在的 observer 对象
        for (auto &observer: queueObservers_) {
            observer.store(nullptr, std::memory_order_release);
        }

        // 调用 QueueObserverFactory::make() 工厂方法创建新的 QueueObserverFactory 对象
        return QueueObserverFactory::make(
                // 传递线程池的名称作为 observer 的名称前缀
                "cpu." + getName(),
                // 传递任务队列的优先级数量 numPriorities
                taskQueue_->getNumPriorities(),
                // 传递 threadIdCollector_ 对象 pointer,用于获取线程 ID 映射
                threadIdCollector_.get());
    }

} // namespace ThreadPool
