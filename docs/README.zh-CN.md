# PACE 中文文档导航

PACE 使用 C++20 协程、独立的事件循环 context 和可选的多线程 runtime，
目前提供数字 IPv4 TCP 与 HTTP/1 服务端。你可以让调用线程直接执行事件循环，
也可以指定工作线程数量，或完全由应用管理线程。

- [循序渐进的中文学习指南](tutorial/README.zh-CN.md)
- [完整英文教程](tutorial/README.md)
- [API 中文索引和关键约定](reference/README.zh-CN.md)
- [完整英文 API 参考](reference/README.md)
- [架构说明](architecture.md)
- [性能测试方法](../benchmarks/README.md)
- [组件测试](../benchmarks/components.md)与[阶段耗时诊断](../benchmarks/profiling.md)

最快的开始方式是在仓库根目录运行：

```sh
xmake f -m release --cxxstandard=cxx20 --examples=y --tests=y -y
xmake -y
xmake test -v
xmake run tutorial_tasks
xmake run tutorial_tcp
xmake run tutorial_http
```

默认教程运行会自行结束并检查结果。TCP、HTTP 的 `serve` 模式会持续提供服务，
可用 Ctrl-C 请求停止。默认示例中的数据和耗时只用于正确性检查，不是性能结论。

请首先记住三个生命周期关系：context 要比其 socket/listener 活得更久；
socket 对象和缓冲区要活到对应异步操作完成；`stop()` 只是请求停止，
只有 `run()` 返回或执行线程被 join 后，才能销毁关联的执行环境。
