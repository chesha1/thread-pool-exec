#include "Executor.h"

#include <stdexcept>

#include <glog/logging.h>

#include <folly/ExceptionString.h>
#include <folly/Portability.h>
#include <folly/lang/Exception.h>

namespace ThreadPool {

    void Executor::invokeCatchingExnsLog(char const *const prefix) noexcept {
        auto ep = std::current_exception();
//        LOG(ERROR) << prefix << " threw unhandled " << folly::exceptionStr(ep);
        LOG(ERROR) << prefix << " threw unhandled ";
    }

//    void Executor::addWithPriority(Func, int8_t /* priority */) {
//        throw std::runtime_error(
//                "addWithPriority() is not implemented for this Executor");
//    }

    bool Executor::keepAliveAcquire() noexcept {
        return false;
    }

//    void Executor::keepAliveRelease() noexcept {
//        LOG(FATAL) << __func__ << "() should not be called for folly::Executor types "
//                   << "which do not override keepAliveAcquire()";
//    }
//
// Base case of permitting with no termination to avoid nullptr tests
    static ExecutorBlockingList emptyList{nullptr, {false, false, nullptr, {}}};

    thread_local ExecutorBlockingList *executor_blocking_list = &emptyList;

    folly::Optional<ExecutorBlockingContext> getExecutorBlockingContext() noexcept {
        return //
                folly::kIsMobile || !executor_blocking_list->curr.forbid ? folly::none : //
                folly::make_optional(executor_blocking_list->curr);
    }

    ExecutorBlockingGuard::ExecutorBlockingGuard(PermitTag) noexcept {
        if (!folly::kIsMobile) {
            list_ = *executor_blocking_list;
            list_.prev = executor_blocking_list;
            list_.curr.forbid = false;
            // Do not overwrite tag or executor pointer
            executor_blocking_list = &list_;
        }
    }

    ExecutorBlockingGuard::ExecutorBlockingGuard(
            TrackTag, Executor *ex, folly::StringPiece tag) noexcept {
        if (!folly::kIsMobile) {
            list_ = *executor_blocking_list;
            list_.prev = executor_blocking_list;
            list_.curr.forbid = true;
            list_.curr.ex = ex;
            // If no string was provided, maintain the parent string to keep some
            // information
            if (!tag.empty()) {
                list_.curr.tag = tag;
            }
            executor_blocking_list = &list_;
        }
    }

    ExecutorBlockingGuard::ExecutorBlockingGuard(
            ProhibitTag, Executor *ex, folly::StringPiece tag) noexcept {
        if (!folly::kIsMobile) {
            list_ = *executor_blocking_list;
            list_.prev = executor_blocking_list;
            list_.curr.forbid = true;
            list_.curr.ex = ex;
            list_.curr.allowTerminationOnBlocking = true;
            // If no string was provided, maintain the parent string to keep some
            // information
            if (!tag.empty()) {
                list_.curr.tag = tag;
            }
            executor_blocking_list = &list_;
        }
    }

    ExecutorBlockingGuard::~ExecutorBlockingGuard() {
        if (!folly::kIsMobile) {
            if (executor_blocking_list != &list_) {
                folly::terminate_with<std::logic_error>("dtor mismatch");
            }
            executor_blocking_list = list_.prev;
        }
    }

} // namespace ThreadPool
