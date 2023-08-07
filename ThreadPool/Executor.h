#pragma once

#include <cassert>
#include <climits>
#include <utility>

#include <folly/Function.h>
#include <folly/Optional.h>
#include <folly/Range.h>
#include <folly/Utility.h>
#include <folly/lang/Exception.h>

// 定义了一个 Executor 的抽象，用于调度和管理任务。
// 它还提供了与任务生命周期和执行器阻塞行为相关的一些工具和机制
namespace ThreadPool {

    // 任何可以被调用，而不需要参数，且不返回值的对象
    using Func = folly::Function<void()>;

    namespace detail {

        // 这是一个基类，主要定义了一些常量和标志，这些标志用于跟踪KeepAlive对象的状态
        class ExecutorKeepAliveBase {
        public:
            //  A dummy keep-alive is a keep-alive to an executor which does not support
            //  the keep-alive mechanism.
            // 代表一个不支持“keep-alive”机制的执行器的标志
            // uintptr_t: 这是一个整数类型，它的大小足以保存一个指针的值。它常常用于整数和指针之间的转换。
            // 将整数 1 转换为 uintptr_t 类型，向左移动 0 位
            // 它的值是第一位（1 << 0）
            static constexpr uintptr_t kDummyFlag = uintptr_t(1) << 0;

            //  An alias keep-alive is a keep-alive to an executor to which there is
            //  known to be another keep-alive whose lifetime surrounds the lifetime of
            //  the alias.
            // 代表一个执行器的标志，该执行器有另一个“keep-alive”对象，其生命周期围绕着这个标志的生命周期
            // 它的值是第二位（1 << 1）
            static constexpr uintptr_t kAliasFlag = uintptr_t(1) << 1;

            // 一个掩码，结合了kDummyFlag和kAliasFlag。
            // 这种掩码用于在不影响其他位的情况下对值中的特定位进行操作
            static constexpr uintptr_t kFlagMask = kDummyFlag | kAliasFlag;

            // 一个掩码，表示除了用于标志的位以外的所有位。
            static constexpr uintptr_t kExecutorMask = ~kFlagMask;
        };

    } // namespace detail

/// An Executor accepts units of work with add(), which should be
/// threadsafe.
    class Executor {
        // 它是一个抽象基类，定义了一个执行器应该有的接口
    public:
        virtual ~Executor() = default;

        /// Enqueue a function to be executed by this executor. This and all
        /// variants must be threadsafe.
        // 将一个函数添加到执行器以供执行
        virtual void add(Func) = 0;

        /// Enqueue a function with a given priority, where 0 is the medium priority
        /// This is up to the implementation to enforce
        // 添加带优先级的任务
        virtual void addWithPriority(Func, int8_t priority);

        virtual uint8_t getNumPriorities() const { return 1; }

        // 定义了三种优先级常量，即低、中、高
        static constexpr int8_t LO_PRI = SCHAR_MIN;
        static constexpr int8_t MID_PRI = 0;
        static constexpr int8_t HI_PRI = SCHAR_MAX;

        /**
         * Executor::KeepAlive is a safe pointer to an Executor.
         * For any Executor that supports KeepAlive functionality, Executor's
         * destructor will block until all the KeepAlive objects associated with that
         * Executor are destroyed.
         * For Executors that don't support the KeepAlive functionality, KeepAlive
         * doesn't provide such protection.
         *
         * KeepAlive should *always* be used instead of Executor*. KeepAlive can be
         * implicitly constructed from Executor*. getKeepAliveToken() helper method
         * can be used to construct a KeepAlive in templated code if you need to
         * preserve the original Executor type.
         */
        // 这是一个嵌套模板类，用于表示一个安全的执行器指针。
        // 确保其指向的执行器不会过早地被析构
        // 对于支持 KeepAlive 功能的执行器，当与该执行器关联的所有 KeepAlive 对象都被销毁时，执行器的析构函数会阻塞。
        // 对于不支持 KeepAlive 功能的执行器，KeepAlive 不提供此类保护
        template<typename ExecutorT = Executor>
        class KeepAlive : private detail::ExecutorKeepAliveBase {
        public:
            // 存储一个可调用的对象，
            // 该对象接受一个类型为 KeepAlive 的右值引用，并返回 void
            using KeepAliveFunc = folly::Function<void(KeepAlive &&)>;

            KeepAlive() = default;

            ~KeepAlive() {
                // 确保 KeepAlive 类具有标准布局
                static_assert(
                        std::is_standard_layout<KeepAlive>::value, "standard-layout");

                // 确保 KeepAlive 类的大小与一个指针的大小相同
                static_assert(sizeof(KeepAlive) == sizeof(void *), "pointer size");

                // 确保 KeepAlive 类的对齐需求与一个指针的对齐需求相同
                static_assert(alignof(KeepAlive) == alignof(void *), "pointer align");

                // 释放与该 KeepAlive 对象关联的任何资源
                reset();
            }

            // 移动构造函数
            KeepAlive(KeepAlive &&other) noexcept
                    : storage_(std::exchange(other.storage_, 0)) {}

            // 拷贝构造函数
            KeepAlive(const KeepAlive &other) noexcept
                    : KeepAlive(getKeepAliveToken(other.get())) {}

            // 使用 SFINAE 技术实现了一些模板条件约束
            // 允许用户创建一个 KeepAlive<ExecutorT> 对象，
            // 从另一个与其兼容的 KeepAlive<OtherExecutor> 对象
            // （即当 OtherExecutor 指针可以被转换为 ExecutorT 指针时）
            template<
                    typename OtherExecutor,
                    typename = typename std::enable_if<
                            // 这里使用了 std::enable_if 来限制这个构造函数
                            // 只有当 OtherExecutor * 是可转换为 ExecutorT * 的时候才是有效的
                            std::is_convertible<OtherExecutor *, ExecutorT *>::value>::type>
            /* implicit */ KeepAlive(KeepAlive<OtherExecutor> &&other) noexcept
                    : KeepAlive(other.get(), other.storage_ & kFlagMask) {
                other.storage_ = 0;
            }

            template<
                    typename OtherExecutor,
                    typename = typename std::enable_if<
                            std::is_convertible<OtherExecutor *, ExecutorT *>::value>::type>
            /* implicit */ KeepAlive(const KeepAlive<OtherExecutor> &other) noexcept
                    : KeepAlive(getKeepAliveToken(other.get())) {}

            /* implicit */ KeepAlive(ExecutorT *executor) {
                *this = getKeepAliveToken(executor);
            }

            // 移动赋值操作符
            KeepAlive &operator=(KeepAlive &&other) noexcept {
                reset();
                storage_ = std::exchange(other.storage_, 0);
                return *this;
            }

            // 拷贝赋值操作符
            KeepAlive &operator=(KeepAlive const &other) {
                return operator=(folly::copy(other));
            }

            template<
                    typename OtherExecutor,
                    typename = typename std::enable_if<
                            std::is_convertible<OtherExecutor *, ExecutorT *>::value>::type>
            KeepAlive &operator=(KeepAlive<OtherExecutor> &&other) noexcept {
                return *this = KeepAlive(std::move(other));
            }

            template<
                    typename OtherExecutor,
                    typename = typename std::enable_if<
                            std::is_convertible<OtherExecutor *, ExecutorT *>::value>::type>
            KeepAlive &operator=(const KeepAlive<OtherExecutor> &other) {
                return *this = KeepAlive(other);
            }

            // 如果 KeepAlive 对象与执行器相关联，并且它不是 dummy 或 alias ，
            // 它会释放 keep-alive 令牌。这是防止执行器过早销毁的机制
            void reset() noexcept {
                if (Executor *executor = get()) {
                    auto const flags = std::exchange(storage_, 0) & kFlagMask;
                    if (!(flags & (kDummyFlag | kAliasFlag))) {
                        executor->keepAliveRelease();
                    }
                }
            }

            // bool 类型转换操作符，使得 KeepAlive 对象可以被用在条件表达式中
            // 由于storage_是一个uintptr_t类型（一个整数类型），
            // 当它为0时，转换结果是false，否则为true
            explicit operator bool() const { return storage_; }

            ExecutorT *get() const {
                // 使用位与运算将 storage_ 与 kExecutorMask 掩码结合，
                // 来确保我们只获取与执行器关联的位
                // 返回一个指向执行器的指针
                return reinterpret_cast<ExecutorT *>(storage_ & kExecutorMask);
            }

            ExecutorT &operator*() const { return *get(); }

            ExecutorT *operator->() const { return get(); }

            // 复制当前的KeepAlive对象。
            // 如果它是一个"dummy"对象，那么返回一个相同的新"dummy"对象；
            // 否则，返回一个与当前对象关联的执行器的新的"keep alive"标记
            KeepAlive copy() const {
                return isKeepAliveDummy(*this) //
                       ? makeKeepAliveDummy(get())
                       : getKeepAliveToken(get());
            }

            // 创建一个新的 KeepAlive 对象，其 storage_ 值与当前对象相同，
            // 但 kAliasFlag 标志已被设置
            KeepAlive get_alias() const { return KeepAlive(storage_ | kAliasFlag); }

            template<class KAF>
            // 函数限定符 && 意味着这个add函数只能在对象的右值上被调用
            void add(KAF &&f) &&{
                static_assert(
                        // 检查 KAF 类型是否可以被调用并接受 KeepAlive && 类型的参数
                        folly::is_invocable<KAF, KeepAlive &&>::value,
                        "Parameter to add must be void(KeepAlive&&)>");
                auto ex = get();

                // 这个 lambda 函数捕获了 KeepAlive 对象（通过移动语义）和参数 f
                ex->add([ka = std::move(*this), f = std::forward<KAF>(f)]() mutable {
                    // 在lambda函数内部，它调用f并传递已捕获的KeepAlive对象
                    f(std::move(ka));
                });

                // ???
                // 这个add函数的整体目的是允许用户在执行器上添加一个回调，
                // 这个回调在被调用时会接收一个KeepAlive对象作为参数。
                // 这使得执行器可以在回调内部安全地使用这个KeepAlive对象，而不需要担心它在回调执行之前被销毁
            }

        private:
            // Executor 类可以访问 KeepAlive 类的所有私有和受保护成员
            friend class Executor;

            // 无论 KeepAlive 类的哪个模板实例，它们都可以访问任何其他 KeepAlive 模板实例的私有和受保护成员。
            // 这样可以确保，一个特化的或具有不同执行器类型的 KeepAlive 可以与另一个 KeepAlive 实例交互，
            // 而不需要通过公共接口
            template<typename OtherExecutor>
            friend
            class KeepAlive;

            // 另一个构造函数，它接收两个参数：
            // 一个 ExecutorT 指针类型的 executor 和一个 uintptr_t 类型的 flags 。
            // 该构造函数的主要目的是将 executor 指针和 flags 合并到单个 uintptr_t 类型的成员变量 storage_ 中
            KeepAlive(ExecutorT *executor, uintptr_t flags) noexcept
                    : storage_(reinterpret_cast<uintptr_t>(executor) | flags) {
                assert(executor);
                assert(!(reinterpret_cast<uintptr_t>(executor) & ~kExecutorMask));
                assert(!(flags & kExecutorMask));
            }

            explicit KeepAlive(uintptr_t storage) noexcept: storage_(storage) {}

            //  Combined storage for the executor pointer and for all flags.
            // 同时存储一个指针和某些标志
            // 初始时，storage_ 仅仅代表一个空指针，没有任何标志位被设置
            uintptr_t storage_{reinterpret_cast<uintptr_t>(nullptr)};
        };

        template<typename ExecutorT>
        static KeepAlive<ExecutorT> getKeepAliveToken(ExecutorT *executor) {
            // 这个函数的目的是为给定的执行器（executor）对象生成一个保活令牌（keep-alive token）
            static_assert(
                    std::is_base_of<Executor, ExecutorT>::value,
                    "getKeepAliveToken only works for folly::Executor implementations.");
            if (!executor) {
                return {};
            }
            Executor *executorPtr = executor;
            if (executorPtr->keepAliveAcquire()) {
                return makeKeepAlive<ExecutorT>(executor);
            }
            return makeKeepAliveDummy<ExecutorT>(executor);
        }

        template<typename ExecutorT>
        static KeepAlive<ExecutorT> getKeepAliveToken(ExecutorT &executor) {
            static_assert(
                    std::is_base_of<Executor, ExecutorT>::value,
                    "getKeepAliveToken only works for folly::Executor implementations.");
            // 调用另一个 getKeepAliveToken 函数重载
            return getKeepAliveToken(&executor);
        }

        template<typename F>
        FOLLY_ERASE static void invokeCatchingExns(char const *p, F f) noexcept {
            catch_exception(f, invokeCatchingExnsLog, p);
        }

    protected:
        /**
         * Returns true if the KeepAlive is constructed from an executor that does
         * not support the keep alive ref-counting functionality
         */

        // 判断对象是否被标记为"dummy"
        template<typename ExecutorT>
        static bool isKeepAliveDummy(const KeepAlive<ExecutorT> &keepAlive) {
            return keepAlive.storage_ & KeepAlive<ExecutorT>::kDummyFlag;
        }

        static bool keepAliveAcquire(Executor *executor) {
            return executor->keepAliveAcquire();
        }

        static void keepAliveRelease(Executor *executor) {
            return executor->keepAliveRelease();
        }

        // Acquire a keep alive token. Should return false if keep-alive mechanism
        // is not supported.
        virtual bool keepAliveAcquire() noexcept;

        // Release a keep alive token previously acquired by keepAliveAcquire().
        // Will never be called if keepAliveAcquire() returns false.
        virtual void keepAliveRelease() noexcept;

        template<typename ExecutorT>
        static KeepAlive<ExecutorT> makeKeepAlive(ExecutorT *executor) {
            // 创建并返回了一个 KeepAlive<ExecutorT> 对象，
            // 并使用指向 ExecutorT 的指针和 uintptr_t(0) 作为参数来初始化它
            static_assert(
                    std::is_base_of<Executor, ExecutorT>::value,
                    "makeKeepAlive only works for folly::Executor implementations.");
            return KeepAlive<ExecutorT>{executor, uintptr_t(0)};
        }

    private:
        static void invokeCatchingExnsLog(char const *prefix) noexcept;

        template<typename ExecutorT>
        static KeepAlive<ExecutorT> makeKeepAliveDummy(ExecutorT *executor) {
            static_assert(
                    std::is_base_of<Executor, ExecutorT>::value,
                    "makeKeepAliveDummy only works for folly::Executor implementations.");
            return KeepAlive<ExecutorT>{executor, KeepAlive<ExecutorT>::kDummyFlag};
        }
    };

/// Returns a keep-alive token which guarantees that Executor will keep
/// processing tasks until the token is released (if supported by Executor).
/// KeepAlive always contains a valid pointer to an Executor.
    template<typename ExecutorT>
    Executor::KeepAlive<ExecutorT> getKeepAliveToken(ExecutorT *executor) {
        static_assert(
                std::is_base_of<Executor, ExecutorT>::value,
                "getKeepAliveToken only works for folly::Executor implementations.");
        return Executor::getKeepAliveToken(executor);
    }

    template<typename ExecutorT>
    Executor::KeepAlive<ExecutorT> getKeepAliveToken(ExecutorT &executor) {
        static_assert(
                std::is_base_of<Executor, ExecutorT>::value,
                "getKeepAliveToken only works for folly::Executor implementations.");
        return getKeepAliveToken(&executor);
    }

    template<typename ExecutorT>
    Executor::KeepAlive<ExecutorT> getKeepAliveToken(
            Executor::KeepAlive<ExecutorT> &ka) {
        return ka.copy();
    }

    struct ExecutorBlockingContext {
        // 定义了一个执行器的阻塞上下文，包括一些标志和指向执行器的指针
        bool forbid;
        bool allowTerminationOnBlocking;
        Executor *ex = nullptr;
        folly::StringPiece tag;
    };
    static_assert(
            std::is_standard_layout<ExecutorBlockingContext>::value,
            "non-standard layout");

    struct ExecutorBlockingList {
        // 表示与阻塞上下文相关的列表
        ExecutorBlockingList *prev;
        ExecutorBlockingContext curr;
    };
    static_assert(
            std::is_standard_layout<ExecutorBlockingList>::value,
            "non-standard layout");

    class ExecutorBlockingGuard {
        // 在其生命周期内提供对 Executor 的阻塞行为的控制。
        // 当你创建一个 ExecutorBlockingGuard 对象时，它会根据所给的标签设置相应的阻塞行为。
        // 当对象被销毁时（例如，离开作用域时），它会清理和还原与阻塞相关的设置
    public:
        struct PermitTag {
        };
        struct TrackTag {
        };
        struct ProhibitTag {
        };

        ~ExecutorBlockingGuard();

        ExecutorBlockingGuard() = delete;

        explicit ExecutorBlockingGuard(PermitTag) noexcept;

        explicit ExecutorBlockingGuard(
                TrackTag, Executor *ex, folly::StringPiece tag) noexcept;

        explicit ExecutorBlockingGuard(
                ProhibitTag, Executor *ex, folly::StringPiece tag) noexcept;

        ExecutorBlockingGuard(ExecutorBlockingGuard &&) = delete;

        ExecutorBlockingGuard(ExecutorBlockingGuard const &) = delete;

        ExecutorBlockingGuard &operator=(ExecutorBlockingGuard const &) = delete;

        ExecutorBlockingGuard &operator=(ExecutorBlockingGuard &&) = delete;

    private:
        // 一个与阻塞行为相关的资源列表，或者是跟踪 Executor 的阻塞状态
        ExecutorBlockingList list_;
    };

    folly::Optional<ExecutorBlockingContext> getExecutorBlockingContext() noexcept;

} // namespace ThreadPool
