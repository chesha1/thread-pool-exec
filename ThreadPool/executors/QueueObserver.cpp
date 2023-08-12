#include "executors/QueueObserver.h"

namespace {

    // 它是一个回退函数（fallback function）。
    // 当某些预期的函数实现（如make_queue_observer_factory）不可用或不存在时，这个回退函数会被使用
    // 如果 make_queue_observer_factory 函数指针没有被正确初始化或赋值，
    // make_queue_observer_factory_fallback 就可以作为一个默认行为来确保程序的稳定性
    std::unique_ptr<ThreadPool::QueueObserverFactory>
    make_queue_observer_factory_fallback(
            const std::string &, size_t, ThreadPool::WorkerProvider *) noexcept {
        return std::unique_ptr<ThreadPool::QueueObserverFactory>();
    }

    // 它持有一个线程退出锁的读取器，当这个对象被销毁时，读取器也会被销毁
    class WorkerKeepAlive : public ThreadPool::WorkerProvider::KeepAlive {
    public:
        explicit WorkerKeepAlive(folly::SharedMutex::ReadHolder idsLock)
                : threadsExitLock_(std::move(idsLock)) {}

        ~WorkerKeepAlive() override {}

    private:
        folly::SharedMutex::ReadHolder threadsExitLock_;
    };

} // namespace

namespace ThreadPool {

    // 创建一个 WorkerKeepAlive 对象，
    // 锁定线程 ID 集合进行读取，然后返回这些 ID
    ThreadIdWorkerProvider::IdsWithKeepAlive
    ThreadIdWorkerProvider::collectThreadIds() {
        auto keepAlive = std::make_unique<WorkerKeepAlive>(
                folly::SharedMutex::ReadHolder{&threadsExitMutex_});
        auto locked = osThreadIds_.rlock();
        return {std::move(keepAlive), {locked->begin(), locked->end()}};
    }

    // 向线程 ID 集合中添加一个 ID
    void ThreadIdWorkerProvider::addTid(pid_t tid) {
        osThreadIds_.wlock()->insert(tid);
    }

    // 从线程 ID 集合中删除一个 ID ，并阻塞，
    // 直到所有的 WorkerKeepAlive 对象都被销毁
    void ThreadIdWorkerProvider::removeTid(pid_t tid) {
        osThreadIds_.wlock()->erase(tid);
        // block until all WorkerKeepAlives have been destroyed
        folly::SharedMutex::WriteHolder w{threadsExitMutex_};
    }

    WorkerProvider::KeepAlive::~KeepAlive() {}

    // 返回一个 QueueObserverFactory 的 std::unique_ptr
    // 根据 make_queue_observer_factory 函数指针是否已经被初始化
    // 来决定使用哪种方法创建 QueueObserverFactory
/* static */ std::unique_ptr<QueueObserverFactory> QueueObserverFactory::make(
            const std::string &context,
            size_t numPriorities,
            WorkerProvider *workerProvider) {
        auto f = make_queue_observer_factory ? make_queue_observer_factory
                                             : make_queue_observer_factory_fallback;
        return f(context, numPriorities, workerProvider);
    }
} // namespace ThreadPool
