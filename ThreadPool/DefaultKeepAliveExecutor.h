#pragma once

#include <future>

#include <glog/logging.h>

#include "Executor.h"
#include "executors/SequencedExecutor.h"
#include "synchronization/Baton.h"

namespace ThreadPool {

/// An Executor accepts units of work with add(), which should be
/// threadsafe.
    // 一个默认的保持活动的执行器（KeepAlive Executor），
    // 这意味着该执行器会保持活跃状态直到明确要求它停止或销毁为止。
    // 此外，通过使用弱引用，允许资源在不再被使用时自动释放
    class DefaultKeepAliveExecutor : public virtual Executor {
    public:
        // 确保 keepAlive_ 在对象被销毁时不应存在
        virtual ~DefaultKeepAliveExecutor() override { DCHECK(!keepAlive_); }

        // getWeakRef 方法被设计为静态的是为了满足某些特定的设计和使用场景。具体来说：
        // 1. 不依赖实例：由于是静态方法，getWeakRef 不需要一个 DefaultKeepAliveExecutor 类的实例就可以被调用。这意味着你可以不创建或不访问一个具体的执行器对象就可以获取一个弱引用。
        // 2. 更大的灵活性：作为一个静态方法，它可以接受任何类型为 DefaultKeepAliveExecutor 或其子类的参数，并返回相应的弱引用。这种设计提供了更大的灵活性，因为它允许你为来自不同子类的执行器对象获取弱引用，而不是限定于一个特定的实例或子类。
        // 3. 明确的目的：getWeakRef 的目标是为其参数（一个执行器对象）创建并返回一个弱引用。如果它不是静态的，那么当你调用它时，你会有两个执行器：调用方法的实例和作为参数传递的执行器。这可能会引起混淆，因为在非静态上下文中，这两者之间的关系并不明确。
        // 4. 避免实例相关的副作用：将 getWeakRef 设计为静态方法可以确保它不会不小心地修改调用它的实例的状态或依赖于其状态。这使得方法更具可预测性和可靠性。
        template<typename ExecutorT>
        static auto getWeakRef(ExecutorT &executor) {
            // 确保传入的 ExecutorT 是 DefaultKeepAliveExecutor 的子类（或它本身）
            static_assert(
                    std::is_base_of<DefaultKeepAliveExecutor, ExecutorT>::value,
                    "getWeakRef only works for folly::DefaultKeepAliveExecutor implementations.");

            // std::conditional_t 根据第一个参数，选择用第二个参数，还是第三个参数
            // 如果 ExecutorT 是 SequencedExecutor 的子类（或它本身），
            // 那么 WeakRefExecutorType 将是 SequencedExecutor；
            // 否则，它将是 Executor
            using WeakRefExecutorType = std::conditional_t<
                    std::is_base_of<SequencedExecutor, ExecutorT>::value,
                    SequencedExecutor,
                    Executor>;

            // 使用先前确定的 WeakRefExecutorType 创建一个 WeakRef，
            // 并返回一个 KeepAlive 对象，它持有对该 WeakRef 的引用
            return WeakRef<WeakRefExecutorType>::create(
                    executor.controlBlock_, &executor);
        }

        // 提供了当前执行器的 KeepAlive 弱引用
        ThreadPool::Executor::KeepAlive<> weakRef() { return getWeakRef(*this); }

    protected:
        void joinKeepAlive() {
            // 确保 DefaultKeepAliveExecutor 适当地释放其持有的资源，
            // 并等待与它相关的其他线程或操作完成

            // 确保 keepAlive_ 是有效的或非空的
            DCHECK(keepAlive_);

            // 释放它所持有的引用或资源
            keepAlive_.reset();

            // 使线程等待直到被 post() 唤醒
            keepAliveReleaseBaton_.wait();
        }

        void joinAndResetKeepAlive() {
            joinKeepAlive();

            // 重置与之相关的状态，并为当前对象创建一个新的 KeepAlive 引用
            auto keepAliveCount =
                    controlBlock_->keepAliveCount_.exchange(1, std::memory_order_relaxed);
            DCHECK_EQ(keepAliveCount, 0);
            keepAliveReleaseBaton_.reset();
            keepAlive_ = makeKeepAlive(this);
        }

    private:
        // 线程安全地计数
        struct ControlBlock {
            std::atomic<ssize_t> keepAliveCount_{1};
        };

        template<typename ExecutorT = Executor>
        class WeakRef : public ExecutorT {
        public:
            // 创建一个 WeakRef 对象，并为其生成一个 KeepAlive 引用
            static ThreadPool::Executor::KeepAlive<ExecutorT> create(
                    std::shared_ptr<ControlBlock> controlBlock, ExecutorT *executor) {
                return makeKeepAlive(new WeakRef(std::move(controlBlock), executor));
            }

            // 首先尝试锁定执行器（确保其是活动的），然后提交任务
            void add(Func f) override {
                if (auto executor = lock()) {
                    executor->add(std::move(f));
                }
            }

            void addWithPriority(Func f, int8_t priority) override {
                if (auto executor = lock()) {
                    executor->addWithPriority(std::move(f), priority);
                }
            }

            virtual uint8_t getNumPriorities() const override { return numPriorities_; }

        private:
            WeakRef(std::shared_ptr<ControlBlock> controlBlock, ExecutorT *executor)
                    : controlBlock_(std::move(controlBlock)),
                      executor_(executor),
                      numPriorities_(executor->getNumPriorities()) {}

            // 这两个方法是关于 KeepAlive 机制的核心。
            // keepAliveAcquire 尝试增加存活计数，而 keepAliveRelease 尝试减少它。
            // 当 KeepAlive 计数减少到1时，WeakRef 对象将被删除

            bool keepAliveAcquire() noexcept override {
                auto keepAliveCount =
                        // fetch_add() 是 std::atomic 的成员函数，通用加法，并返回先前保有的值
                        // 还有 store(), load(), exchange() 等
                        keepAliveCount_.fetch_add(1, std::memory_order_relaxed);
                // We should never increment from 0
                DCHECK(keepAliveCount > 0);
                return true;
            }

            void keepAliveRelease() noexcept override {
                auto keepAliveCount =
                        keepAliveCount_.fetch_sub(1, std::memory_order_acq_rel);
                DCHECK(keepAliveCount >= 1);

                if (keepAliveCount == 1) {
                    delete this;
                }
            }

            // 尝试获取执行器的锁。
            // 如果执行器是活跃的（其存活计数大于0），那么它将返回一个 KeepAlive 引用；
            // 否则，它将返回一个空引用
            ThreadPool::Executor::KeepAlive<ExecutorT> lock() {
                auto controlBlock =
                        controlBlock_->keepAliveCount_.load(std::memory_order_relaxed);
                do {
                    if (controlBlock == 0) {
                        return {};
                    }
                } while (!controlBlock_->keepAliveCount_.compare_exchange_weak(
                        controlBlock,
                        controlBlock + 1,
                        std::memory_order_release,
                        std::memory_order_relaxed));

                return makeKeepAlive<ExecutorT>(executor_);
            }

            std::atomic<size_t> keepAliveCount_{1};

            std::shared_ptr<ControlBlock> controlBlock_;
            ExecutorT *executor_;

            uint8_t numPriorities_;
        };

        bool keepAliveAcquire() noexcept override {
            auto keepAliveCount =
                    controlBlock_->keepAliveCount_.fetch_add(1, std::memory_order_relaxed);
            // We should never increment from 0
            DCHECK(keepAliveCount > 0);
            return true;
        }

        void keepAliveRelease() noexcept override {
            auto keepAliveCount =
                    controlBlock_->keepAliveCount_.fetch_sub(1, std::memory_order_acq_rel);
            DCHECK(keepAliveCount >= 1);

            if (keepAliveCount == 1) {
                keepAliveReleaseBaton_.post(); // std::memory_order_release
            }
        }

        std::shared_ptr<ControlBlock> controlBlock_{std::make_shared<ControlBlock>()};
        Baton<> keepAliveReleaseBaton_;

        // 创建了一个 KeepAlive 对象，
        // 该对象的作用是保持 DefaultKeepAliveExecutor 的实例在某段时间内保持活动状态
        // 可能是为了确保 DefaultKeepAliveExecutor 对象在某些异步操作中不会被提前销毁，
        // 从而确保异步操作的正确完成
        KeepAlive <DefaultKeepAliveExecutor>  keepAlive_{makeKeepAlive(this)};
    };

    template<typename ExecutorT>
    auto getWeakRef(ExecutorT &executor) {
        static_assert(
                std::is_base_of<DefaultKeepAliveExecutor, ExecutorT>::value,
                "getWeakRef only works for folly::DefaultKeepAliveExecutor implementations.");
        return DefaultKeepAliveExecutor::getWeakRef(executor);
    }

} // namespace ThreadPool

