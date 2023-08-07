#include <iostream>
#include <folly/Function.h>

int main() {
    using Func = folly::Function<void()>;
    std::cout << "main";
    return 0;
}