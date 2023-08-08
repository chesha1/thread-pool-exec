#pragma once

#include "Executor.h"

namespace ThreadPool {

// SequencedExecutor is an executor that provides the following guarantee:
// if add(A) and add(B) were sequenced (i.e. add(B) was called after add(A) call
// had returned to the caller) then execution of A and B will be sequenced
// (i.e. B() can be called only after A() returns) too.
// 如果两个任务是按顺序添加到此执行器的，那么它们的执行也将是有序的
    class SequencedExecutor : public virtual Executor {
    public:
        // 虚析构函数
        // 明确指示编译器这个函数旨在重写基类中的一个虚函数。
        // 如果基类中没有匹配的虚函数，使用 override 关键字会导致编译错误
        virtual ~SequencedExecutor() override {}
    };

} // namespace ThreadPool
