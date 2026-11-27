# Explanation

[简体中文](README.zh.md) · [Documentation](../README.md)

Why PACE is built the way it is. These pages explain design and trade-offs; for how to use an API see the
[reference](../reference/README.md), and for step-by-step learning the [tutorial](../tutorial/README.md).

| Page | Explains |
|:--|:--|
| [Architecture](architecture.md) | The modules and their layers, reactors and readiness backends, tasks, thread affinity and shutdown |
| [Execution model](execution-model.md) | Where code runs: contexts, the four ways to run them, placing work, cooperative scheduling, stopping |
| [Ownership and lifetimes](ownership.md) | Who owns what: lazy tasks, coroutine frames, lambdas, sockets, results and shared state |
| [API design](api-design.md) | Why the API has direct calls, one-line entry points and explicit forms side by side |
