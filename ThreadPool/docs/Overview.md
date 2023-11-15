# Overview
Some parts of this markdown file is copied and translated from docs of folly.

- [Overview](#overview)
  - [项目结构](#项目结构)
  - [Baton.h](#batonh)
  - [Futex.h](#futexh)
  - [MPMCQueue.h](#mpmcqueueh)
  - [Executors 和 线程池](#executors-和-线程池)
    - [线程池](#线程池)


## 项目结构
[思维导图](https://amymind.com/view/mindmap/LxpOPg2eKMQ4r9m3)

## Baton.h
`Baton`允许线程阻塞一次然后被唤醒：它捕捉一个单独的切换  
它本质上是一个非常小、非常快的信号量，只支持对于`sem_call`和`sem_wait`的一次单独调用  

在其生命周期（从构造/重置到销毁/重置）中，`Baton`必须分别被`post()`和`wait()`一次，或者根本不被`post()`和`wait()`。
Baton 不包含内部填充，并且大小只有 4 个字节。避免伪共享的任何对齐或填充都取决于用户。  

非阻塞版本（`MayBlock == false`）通过在关键路径中仅使用 load acquire 和 store release 来提供更高的速度，
但代价是不允许阻塞。当前的 posix 信号量`sem_t`还不错，但这提供了更快的速度、内联、更小的尺寸、保证实现不会改变以及与确定性调度的兼容性。
有更加严格的生命周期，考虑到添加了一堆断言，这有助于提前捕获竞争条件。
带有`MayBlock == false`的`Baton` `post`是异步信号安全的。
当`MayBlock == true`时，如果 Futex 唤醒是这样，则 Baton post 是异步信号安全的。

## Futex.h
`Futex`是一个原子的 32 位无符号整数，提供对该值的`futex()`系统调用的访问。
它被模板化方式，这样它就可以与`DeterministicSchedule`测试正确交互。
如果您不知道如何使用`futex()`，您可能不应该使用此类。
即使您确实知道如何操作，您也应该有充分的理由（以及支持您的基准测试）。
由于`futex`系统调用的语义，`futex`系列函数可作为自由函数而不是成员函数调用
## MPMCQueue.h
`MPMCQueue`是高性能的有界并发队列，支持多生产者、多消费者和可选的阻塞。队列有固定长度，所有内存事先分配。
大批量的入队和出队操作可以并行执行。

`MPMCQueue`是线性的。这意味着，如果对`write(A)`的调用在对`write(B)`的调用开始之前返回，
则`A`肯定会在`B`之前进入队列，并且如果对`read(X)`的调用在对`read(Y)`的调用开始之前返回，
`X`将是队列中比`Y`更早的元素。 这也意味着，如果读取调用返回一个值，您可以确定队列中所有先前的元素都已分配了一个读取器（该读取器可能尚未返回，但它存在）。

底层的实现对头部和尾部使用 ticket 分配器，将访问分散到 N 个单元素队列中，以生成容量为 N 的队列。
ticket 分配器原子地增长，面对竞争，这比 CAS 循环更稳定。每个单元素队列都使用自己的 CAS 来序列化访问，并具有自适应自旋截止。
当单元素队列上的旋转失败时，即使单个队列上存在多个等待者（例如当`MPMCQueue`的容量小于入队或出队的数量时），它也会使用`futex()`的`_BITSET`操作来减少不必要的唤醒。

性能测试：。。。性能很好，因为它使用`futex()`来阻塞和取消阻塞等待线程，而不是使用`sched_yield`进行自旋。

NOEXCEPT

如果您有一个由 N 个队列消费者组成的池，您希望在队列耗尽后关闭该池，一种方法是将 N 个哨兵值排入队列。
如果生产者不知道有多少个消费者，您可以将一个哨兵入队，然后让每个消费者在收到它后重新排队两个哨兵（通过重新排队 2 个，关闭可以在 O(log P) 时间内完成，而不是 O(P) 时间 ）。

`MPMCQueue`的动态版本已经被废弃，建议使用`UnboundedQueue`

我们按升序分配 ticket，但我们不想访问`slot_`的相邻元素，因为这会导致错误共享（多个核心访问同一缓存行，即使它们没有访问该缓存行中的相同字节）。
为了避免这种情况，每个 ticket 有比较大的步长。




附加的工具`MPMCPipeline.h`是一个扩展，可让你将多个队列及其之间的处理步骤链接在一起。




## Executors 和 线程池
让你的并发代码高效运行

### 线程池
#### 我应该如何使用线程池
Wangle 提供了两种具体的线程池（`IOThreadPoolExecutor`, `CPUThreadPoolExecutor`），并将它们构建为完整异步框架的一部分  
一般来说，您可能想要获取全局执行器，并将其与`future`一起使用，如下所示：
```c++
auto f = someFutureFunction().via(getCPUExecutor()).then(...)
```
或者也许您需要构建一个 thrift/memcache 客户端，并且需要一个 event base：
```c++
auto f = getClient(getIOExecutor()->getEventBase())->callSomeFunction(args...)
         .via(getCPUExecutor())
         .then([](Result r){ .... do something with result});

```

#### 和 C++11 的 std::launch 相比
当前的 C++11 `std::launch`只有两种模式：异步或延迟。
在生产系统中，这两种情况都不是您想要的：async 将为每次 launch 无限制地启动一个新线程，而 deferred 会惰性地将工作延迟到需要时。
但 then 在需要时在当前线程中同步执行工作。


Wangle 的线程池总是尽快启动工作，对允许的最大任务/线程数量有限制。
因此我们永远不会使用超过绝对需要的线程。请参阅下面有关每种类型的执行器的实现详细信息。

#### 为什么我们还需要一组线程池？
不幸的是，现有的线程池都不具备所需的所有功能——基于管道的东西太慢了。
一些较旧的线程池不支持`std::function`

#### 为什么我们需要几种不同类型的线程池？
如果你想要 epoll 支持，你需要一个文件描述符——event_fd 是最新的通知热点。
不幸的是，一个活动的 fd 会触发它所在的所有 epoll 循环，从而导致惊群问题。
所以如果你想要一个公平的队列（总共一个队列 vs 每个工作线程一个队列），你需要使用某种信号量。
不幸的是，信号量不能放入 epoll 循环中，因此它们与 IO 不兼容。
幸运的是，您通常希望将 IO 和 CPU 密集型工作分开，以便为 IO 提供更强的尾延迟(tail latency)保证。

#### IOThreadPoolExecutor
- 使用 event_fd 进行通知，并唤醒 epoll 循环。
- 每个线程/epoll 有一个队列（具体来说，是 NotificationQueue）
- 如果线程已经在运行并且不在 epoll 上等待，我们不会进行任何额外的系统调用来唤醒循环，只需将新任务放入队列中即可
- 如果任何线程等待的时间超过几秒，它的栈就会被 madvise away。然而，目前任务是在队列上循环调度的，因此除非没有工作正在进行，否则这不是很有效
- ::getEventBase() 将返回一个 EventBase，您可以直接在上面安排 IO 工作，可以选择循环。
- 由于每个线程有一个队列，因此队列上几乎不存在任何竞争 - 因此为了任务，用了一个围绕 std::deque 的简单自旋锁。没有最大队列大小。
- 默认情况下，每个核心有一个线程 - 假设它们不阻塞，那么拥有比这更多的 IO 线程通常没有意义。

#### CPUThreadPoolExecutor
- 由 folly/LifoSem 和 folly/MPMC 队列支持的单个队列。由于只有一个队列，因此竞争可能会非常严重，因为所有工作线程和所有生产者线程都访问同一个队列。MPMC 队列在这种情况下表现出色。MPMC 队列规定了最大队列大小。
- LifoSem 按 Lifo 顺序唤醒线程 - 也就是说，只有少数线程需要运行，并且我们总是尝试重用相同的少数线程以获得更好的缓存局部性。
- 不活动的线程的栈被 madvise away。这与 Lifosem 结合使用效果非常好 - 如果在启动时指定的线程数多于所需的线程数，几乎没有影响。
- stop() 将在退出时完成所有未完成的任务
- 支持优先级 - 优先级作为多个队列实现 - 每个工作线程首先检查最高优先级队列。线程本身没有设置优先级，因此一系列长时间运行的低优先级任务仍然可能占用所有线程。（最后检查 pthreads 线程优先级效果不太好）

#### ThreadPoolExecutor
包含线程启动/关闭/统计逻辑的基类，这与任务的实际运行方式脱钩

#### Observers
提供观察者接口来侦听线程启动/停止事件。这对于创建每个线程一个的对象很有用，而且在线程池中添加/删除线程时也能让它们正常工作。

#### Stats
提供 PoolStats 来获取任务计数、运行时间、等待时间等。