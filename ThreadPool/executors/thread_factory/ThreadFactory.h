#pragma once

#include <string>
#include <thread>

#include "Executor.h"

namespace ThreadPool {

// 抽象工厂类，用于创建新的线程
    class ThreadFactory {
    public:
        ThreadFactory() = default;

        virtual ~ThreadFactory() = default;

        // 用于创建新的线程
        virtual std::thread newThread(Func &&func) = 0;

        // 获取线程名的前缀
        virtual const std::string &getNamePrefix() const = 0;

    private:
        ThreadFactory(const ThreadFactory &) = delete;

        ThreadFactory &operator=(const ThreadFactory &) = delete;

        ThreadFactory(ThreadFactory &&) = delete;

        ThreadFactory &operator=(ThreadFactory &&) = delete;
    };

} // namespace ThreadPool

