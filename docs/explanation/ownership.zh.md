<!-- source-hash: 90ed18ee5c52 -->
# 所有权与生命周期

[English](ownership.md) · [设计说明](README.zh.md)

PACE 从不在你背后让某个对象继续存活。套接字、缓冲区和处理函数上都没有隐藏的引用计数：谁持有一个值，谁就拥有它，
借用在类型上一目了然。本页解释由此得出的规则，以及这些规则能避免的错误。规则本身汇总在
[运行时参考](../reference/runtime/README.zh.md)中；具体机制见[架构](architecture.zh.md#任务与对象所有权)。

## 为什么要显式所有权

隐式共享（每个连接都包一层 `shared_ptr`，回调再把它捕获进去）用起来方便，但每次交接都要一次原子加减，每个对象
都要一次堆分配，更糟的是，没人能从代码里读出对象的生命周期。一个“只要还有人引用它就一直存活”的连接，其关闭
顺序是偶然决定的。PACE 改用协程帧：服务某个连接的任务，其协程帧拥有套接字、缓冲区和解析出的请求；任务结束时，
它们按确定的顺序被销毁。

## 任务是惰性的，只能消费一次

调用协程函数会创建它的协程帧，但什么也不执行。任务在被等待或被 spawn 时才开始运行，而这两种做法都会消费它。
`task` 只能移动，左值形式的任务不能被等待：

```cpp
auto pending = stream.read_some(buffer);
co_await std::move(pending);   // 显式写出：这个任务在这里被用掉
```

正是惰性让其余一切变得可预测：在所有者做出决定之前什么都不会运行，所以永远不会有任务抢在本该拥有其结果的代码
前面跑起来。

## 协程帧拥有什么

**按值**传递的参数被复制或移动进协程帧，与任务同样长寿。按引用、指针、`std::span` 或 `std::string_view` 传递的
参数则不然：协程帧只持有引用，被引用的对象必须比任务活得久。

```cpp
pace::task<void> send(pace::tcp::stream& stream, std::string_view text);

std::string greeting = make_greeting();
co_await send(stream, greeting);          // 没问题：greeting 比被等待的任务活得久

auto task = send(stream, make_greeting()); // 错误：临时字符串在这一行就销毁了，
co_await std::move(task);                  // 早于这个惰性任务第一次运行
```

由于任务是惰性的，“参数在调用期间有效”是不够的：它必须一直有效，直到任务结束。拥有所有权的参数则根本不存在这个
问题。PACE 自己的 API 两种都提供：`stream.write_all(text)` 借用数据，应当立即等待；而
`pace::write(stream, std::string)` 拥有自己的字节，可以先存起来，也可以 spawn。

## 协程 lambda

lambda 的捕获存放在闭包对象里，而不是协程帧里。如果闭包是一个临时对象，协程在第一次挂起之后读到的就是已经销毁的
捕获：

```cpp
io.spawn([&config]() -> pace::task<void> { co_await serve(config); }());  // 错误：闭包在分号处就销毁了
```

优先使用带值参数的具名协程函数。在用 lambda 更方便的地方，把它交给会保管它的东西：`pace::run(lambda)` 会持有这个
可调用对象直到关闭完成，这也是它接受 lambda 本身而不是 `lambda()` 的原因。

## 套接字与监听器

- 只要套接字或监听器上还有挂起的操作，它就必须保持地址不变：在它的所有操作完成之前，不要移动或销毁它。
- 在它自己的上下文上关闭它。`close()` 会取消它尚未完成的操作，这些操作以 `std::errc::operation_canceled` 恢复；
  等它们展开之后再销毁对象。
- `io_context` 必须比在它上面创建的每个套接字、监听器和定时器活得久。
- 因此应用的拆除顺序是：停止上下文，join 运行它的线程，销毁套接字和监听器，销毁上下文。`pace::run` 和
  `runtime::run` 会替你完成前两步。

## 结果拥有自己的数据

返回数据的调用都按值返回：直接客户端返回一个完整的 `response`，`udp::exchange` 返回数据报，`pace::run` 返回入口
任务的值。返回的任何东西都不会指向已经停止的上下文，或者将被复用的缓冲区。视图（`std::span`、`std::string_view`）
只作为参数出现，由调用者控制其生命周期，从不作为比调用更长寿的结果。

## 工作线程之间的共享状态

HTTP 处理函数和其他回调按值存放在使用它们提供服务的任务里。这些处理函数通过引用或指针访问的状态，例如缓存或
计数器，属于应用自己；多个工作线程同时使用时，它的同步也由应用负责：PACE 不会替你加任何锁。优先使用每个工作线程
各自的状态，这样不需要锁，只共享必须共享的部分。

## 参见

- [执行模型](execution-model.zh.md)
- [运行时参考](../reference/runtime/README.zh.md)、[`task`](../reference/runtime/task.zh.md)
- 教程：[任务与定时器](../tutorial/04-tasks-and-timers.zh.md)
