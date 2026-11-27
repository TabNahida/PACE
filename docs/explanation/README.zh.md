<!-- source-hash: ba435284eca4 -->
# 设计说明

[English](README.md) · [文档](../README.zh.md)

PACE 为什么是现在这个样子。这些页面解释设计思路与取舍；如何使用某个 API 请查阅[参考](../reference/README.zh.md)，
循序渐进的学习请看[教程](../tutorial/README.zh.md)。

| 页面 | 说明的内容 |
|:--|:--|
| [架构](architecture.zh.md) | 模块及其分层、反应器与就绪通知后端、任务、线程亲和性与关闭 |
| [执行模型](execution-model.zh.md) | 代码在哪里运行：上下文、运行上下文的四种方式、工作的放置、协作式调度、停止 |
| [所有权与生命周期](ownership.zh.md) | 谁拥有什么：惰性任务、协程帧、lambda、套接字、结果与共享状态 |
| [API 设计](api-design.zh.md) | 为什么 API 同时提供直接调用、一行式入口和显式形式 |
