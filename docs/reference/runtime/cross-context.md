# Cross-context communication

[简体中文](cross-context.zh.md) · [Runtime](README.md)

## Overview

Tools for work that spans contexts: a bounded channel that any thread can send into and one context receives from,
a snapshot that publishes an immutable value to every context, `on()` to await work on another context, and a fixed
key-to-context mapping on `runtime`.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/runtime/channel.hpp>` (also `<pace/runtime.hpp>`) | `pace_runtime` | always built |
| `<pace/runtime/snapshot.hpp>` (also `<pace/runtime.hpp>`) | `pace_runtime` | always built |
| `<pace/runtime/on.hpp>` (also `<pace/runtime.hpp>`) | `pace_runtime` | always built |

Each context still runs its own coroutines on its own thread. These tools move values and resumptions between
contexts; they never move a coroutine. A sender, a reader or a caller always resumes on the context it was running
on.

| Need | Use |
|:--|:--|
| Many producers feed state owned by one context (a peer table, a TUN device, quotas) | `channel<T>` owned by that context |
| Every context reads the same configuration, replaced now and then | `snapshot<T>` |
| A result computed on another context, as if it were a local `co_await` | `co_await on(context, work)` |
| Choose the context that owns a key | `runtime::shard_for(key)` ([`runtime`](runtime.md#runtime)) |

## Synopsis

```cpp
#include <pace/runtime.hpp>

#include <atomic>
#include <iostream>
#include <memory>
#include <string>

struct routes
{
    std::string upstream;
};

int main()
{
    pace::runtime workers(4);
    auto& owner = workers.shard_for(std::string("peer-table"));
    pace::channel<int> updates(owner, 1024); // received on `owner`, sent from anywhere
    pace::snapshot<routes> config(workers, std::make_shared<const routes>(routes{"10.0.0.1"}));
    std::atomic<int> producers{3};

    auto consumer = [&]() -> pace::task<void>
    {
        int total = 0;
        while (auto value = co_await updates.receive()) // std::nullopt once closed and drained
        {
            total += *value;
        }
        // Work on another context; resumes here with its result.
        auto upstream = co_await pace::on(
            workers.context(0),
            [&]() -> pace::task<std::string> { co_return config.get()->upstream; }
        );
        std::cout << total << ' ' << upstream << '\n';
        workers.stop();
    };
    auto producer = [&](int value) -> pace::task<void>
    {
        co_await updates.send(value); // waits while the channel is full
        config.publish(std::make_shared<const routes>(routes{"10.0.0.2"}));
        if (--producers == 0)
        {
            updates.close();
        }
    };
    owner.spawn(consumer());
    for (int i = 1; i <= 3; ++i)
    {
        workers.context(static_cast<std::size_t>(i)).spawn(producer(i));
    }
    workers.run();
}
```

## Types

### `overflow_policy`, `send_status`

```cpp
enum class overflow_policy
{
    reject,
    drop_oldest,
};

enum class send_status
{
    sent,
    dropped_oldest,
    full,
    closed,
};
```

| Value | Meaning |
|:--|:--|
| `overflow_policy::reject` | A full channel refuses the value: `try_send` returns `full`; `send()` and `blocking_send()` wait for room. The default. |
| `overflow_policy::drop_oldest` | A full channel discards its oldest queued value to make room. No send ever waits. |
| `send_status::sent` | The value was queued, or handed straight to the waiting `receive()`. |
| `send_status::dropped_oldest` | The value was queued after the oldest queued value was discarded. |
| `send_status::full` | Not queued: the channel is full (only `try_send` under `reject`). |
| `send_status::closed` | Not queued: the channel is closed. |

### `channel`

```cpp
template <class T>
class channel
{
public:
    using value_type = T;

    channel(io_context& owner, std::size_t capacity, overflow_policy overflow = overflow_policy::reject);
    channel(const channel&) = default;
    channel& operator=(const channel&) = default;
    channel(channel&&) noexcept = default;
    channel& operator=(channel&&) noexcept = default;
    ~channel() = default;

    send_status try_send(T&& value);
    send_status try_send(const T& value);
    /* awaitable */ send(T value) noexcept;                     // co_await: send_status
    send_status blocking_send(T value, const cancellation_token& cancellation = {});
    /* awaitable */ receive() noexcept;                         // co_await: std::optional<T>
    std::optional<T> try_receive();
    void close() noexcept;
    bool closed() const;
    std::size_t size() const;
    std::size_t capacity() const noexcept;
};
```

A bounded multi-producer, single-consumer queue. Its values are received by coroutines on `owner`; anything may
send. A `channel` is a handle: copies share one queue, which lives until the last copy is destroyed. `T` must be
nothrow move constructible and nothrow destructible.

| Member | Behaviour |
|:--|:--|
| `channel(owner, capacity, overflow)` | A queue of `capacity` values; all slots are allocated here. Thread-safe; `owner` need not be running yet. Throws `std::invalid_argument` for a capacity of 0. A channel created after `owner` shut down starts closed. |
| `try_send(value)` | From any thread, never waits for room. Returns `sent`, `dropped_oldest`, `full` or `closed`. The rvalue overload moves from `value` only on `sent` or `dropped_oldest`, so a refused value can be retried; the `const T&` overload sends a copy. Resumes a pending `receive()` on the owner. |
| `co_await send(value)` | From a coroutine on any running context, the owner included. Under `reject`, a full channel suspends the sender until the receiver makes room; waiting senders are served in order, each value moved into the slot the receiver freed. Returns `sent`, `dropped_oldest` or `closed`; on `closed` the value is destroyed. The sender resumes on its own context. |
| `blocking_send(value, cancellation)` | From a thread that runs no PACE context. Like `send()`, but the thread blocks while it waits. `cancellation` ends the wait (request or deadline). |
| `co_await receive()` | On the owner only. The oldest value, or `std::nullopt` once the channel is closed and empty. Waits while the channel is empty. One receive at a time. |
| `try_receive()` | On the owner only. The oldest value, or `std::nullopt` when the channel is empty. Also works while the owner drains after `stop()`. |
| `close()` | From any thread; idempotent. Later sends return `closed`; waiting senders resume with `closed`; a pending receive resumes with `std::nullopt`. Values already queued stay receivable. |
| `closed()`, `size()`, `capacity()` | Whether the channel is closed, the number of queued values (it may change at once), and the capacity. Thread-safe. |

`try_send` and the start of `send` take a short internal lock and, when a receive is waiting, post its resumption to
the owner: they never wait for the receiver. `send()` and `receive()` hand values over directly when the other side
is already waiting.

### `snapshot`

```cpp
template <class T>
class snapshot
{
public:
    explicit snapshot(runtime& workers, std::shared_ptr<const T> initial = nullptr);
    explicit snapshot(const std::vector<io_context*>& contexts, std::shared_ptr<const T> initial = nullptr);
    snapshot(const snapshot&) = default;
    snapshot& operator=(const snapshot&) = default;
    snapshot(snapshot&&) noexcept = default;
    snapshot& operator=(snapshot&&) noexcept = default;
    ~snapshot() = default;

    const std::shared_ptr<const T>& get() const;
    std::shared_ptr<const T> load() const;
    void publish(std::shared_ptr<const T> value);
};
```

RCU-style publication of an immutable value to a fixed set of contexts: every context of a `runtime`, or the listed
ones. Each context keeps its own cached `std::shared_ptr<const T>`. `publish()` stores the new value and asks each
context to refresh its copy on its next turn; readers never wait for a publisher and a publisher never waits for
readers. A `snapshot` is a handle: copies share the value and the caches.

| Member | Behaviour |
|:--|:--|
| `snapshot(workers, initial)` | Publishes `initial` (possibly null) to every context of `workers`. Thread-safe; the contexts need not be running. |
| `snapshot(contexts, initial)` | The same for the listed contexts. Throws `std::invalid_argument` for an empty list, a null entry or a context listed twice. |
| `get()` | On one of the snapshot's contexts: that context's copy, with no lock, no atomic read-modify-write and no reference-count change. The reference stays valid until the calling coroutine next suspends; copy the `shared_ptr` to keep the value longer. Linear in the number of contexts. Throws `std::logic_error` on any other thread. |
| `load()` | From any thread: the value published last, as a new `shared_ptr`. It takes the publisher lock; prefer `get()` on a context. |
| `publish(value)` | From any thread: replaces the value. When called on one of the snapshot's contexts, that context sees it at once; every other context sees it after its next turn. Several publishes in quick succession may reach a context as one refresh to the latest. Stopped contexts are not refreshed. |

A context sees the values it observes in publication order. Two contexts may briefly disagree: one may still read
the previous value while another already reads the new one. The previous value is released on the context that
drops the last reference to it.

## Functions

### `on`

```cpp
template <class Factory>
requires(std::is_invocable_v<Factory&, io_context&> || std::is_invocable_v<Factory&>)
auto on(io_context& target, Factory factory);   // task<operation_result<Factory>>

template <class T>
auto on(io_context& target, task<T> work);      // task<T>
```

Runs the work on `target` and resumes the awaiting coroutine on its own context with the result or the exception.
The factory (taking `io_context&`, which is `target`, or nothing) is moved to `target`, invoked there, owned until
the work finishes and destroyed there before the caller resumes, like a [`task_group`](structured-concurrency.md)
child. The `task` overload moves an already-created task's frame to `target`; what the task borrowed stays borrowed.

The work runs under a token linked to the caller's, with the caller's deadline. The caller's cancellation, its
deadline and its context's `stop()` all cancel the work, and the caller always waits for the work to finish: an
`on()` never abandons running work. Awaited on `target` itself, `on()` simply awaits the factory's task.

`on()` returns a lazy task: nothing is sent to `target` until it is awaited. Await it on a running context.

## Errors

| Error | When |
|:--|:--|
| `std::invalid_argument` | `channel` with capacity 0; `snapshot` with an empty, null or repeated context list |
| `std::logic_error` | `receive()` or `try_receive()` off the owner, a second concurrent `receive()`; `send()` awaited on a thread that runs no context; `blocking_send()` on a thread that runs a context; `snapshot::get()` on a thread that runs none of its contexts |
| `std::system_error`, `operation_canceled` | A waiting `send()` or `receive()` whose task token is cancelled or whose context stops; `receive()` once the owner is stopped; `blocking_send()` whose token is cancelled; `on()` when `target` is stopped (the work never runs), when `target` is destroyed before the work finished, or when the caller's token or context cancels the work and the work ends with it |
| `std::system_error`, `timed_out` | The same waits ended by a deadline |
| The work's exception | `on()` rethrows whatever the work threw, on the caller's context |
| `send_status::closed` | Not an exception: sends to a closed channel, including one closed by its owner's shutdown |

A cancelled `send()` did not queue its value. A `send()` that returned `sent` has queued its value even if its token
was cancelled at the same time; the enclosing task's awaiter may still report the cancellation afterwards.

## Threads and concurrency

| Call | From |
|:--|:--|
| `channel` constructor, copies, `try_send`, `close`, `closed`, `size`, `capacity` | any thread |
| `co_await channel::send` | a coroutine on any running context |
| `channel::blocking_send` | a thread that runs no context |
| `co_await channel::receive`, `channel::try_receive` | the owner's thread; one receive at a time |
| `snapshot` constructor, copies, `load`, `publish` | any thread |
| `snapshot::get` | the thread of one of its contexts |
| `co_await on(target, …)` | a coroutine on any running context; any number at once, to any targets |

A thread that runs a context must never block, so it cannot use `blocking_send`. Senders and callers that wait
suspend only themselves.

## Lifetime and ownership

- **The owner's shutdown closes a channel.** When the owner stops, at the start of its drain (or when it is
  destroyed without having run), the channel closes: a pending `receive()` fails with `operation_canceled`, waiting
  senders resume with `closed`, blocked threads return `closed`, and later sends return `closed`. Values still
  queued are destroyed with the last handle. A `channel` may outlive its owner; it only reports `closed`.
- **A handle must outlive its co_await.** `send()` and `receive()` refer to the channel through the handle they were
  called on; keep that handle alive until the `co_await` completes. Other handles can be destroyed at any time.
- **A context that is destroyed takes its waiters with it.** A coroutine suspended in `send()`, `receive()` or
  `on()` whose context finishes its drain before the resumption arrives, or is destroyed, is destroyed with the
  context's other suspended frames, without resuming. The channel or the remote work never touches it afterwards.
- **`on()` work should own its data.** The caller waits for the work, so borrowing from the caller's frame is safe
  while the caller's context runs. If the caller's context stops, the work is cancelled, but the caller can be
  destroyed with its context before the work has finished: capture by value, or keep shared data alive with the
  target.
- **A snapshot reference ends at the next suspension.** `get()` returns a reference to the context's cache, which
  the next refresh replaces between turns; copy the `shared_ptr` to keep a value across a `co_await`.
- **`snapshot` and `channel` keep only a handle to each context**, never the context itself; a context they refer to
  may be destroyed first.

## Limits and defaults

| Item | Value |
|:--|:--|
| Channel capacity | at least 1; all `capacity` slots are allocated at construction |
| Default overflow policy | `overflow_policy::reject` |
| Pending receives per channel | 1 |
| Waiting senders | unbounded; each waits in its own coroutine frame or thread |
| Pending refreshes per snapshot context | at most 1; more publishes merge into it |
| `snapshot::get()` cost | a scan of the snapshot's contexts |

## See also

- [`runtime`](runtime.md): the contexts, and `shard_for` to choose one per key
- [`io_context`](io-context.md): `spawn` and `post`, the lower-level cross-thread entry points
- [Structured concurrency](structured-concurrency.md): tokens, deadlines and how `on()` resembles a child task
- [Execution model](../../explanation/execution-model.md#placing-work)
