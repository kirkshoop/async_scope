---
title: "async_scope - Creating scopes for non-sequential concurrency"
subtitle: "Draft Proposal"
document: D2519R0
date: today
audience:
  - "SG1 Parallelism and Concurrency"
  - "LEWG Library Evolution"
author:
  - name: Kirk Shoop
    email: <kirk.shoop@gmail.com>
  - name: Lee Howes
    email: <lwh@fb.com>
toc: true
---

Changes
=======

## R0

- first revision

Introduction
============

A major precept of [@P2300R2] is structured concurrency. The `start_detached` and `ensure_started` algorithms are motivated by some important scenarios. Not every asynchronous operation has a clear chain of work to consume or block on the result. The problem with these algorithms is that they provide unstructured concurrency. This is an unnecessary and unwelcome and undesirable property for concurrency. It leads to problems with lifetimes, and it requires execution contexts to conflate task lifetime management with execution management.

This paper describes an object that would be used to create a scope that will contain all senders spawned within its lifetime. These senders can be running on any execution context. The scope object has only one concern, which is to contain the spawned senders to a lifetime that is nested within any other resources that they depend on. In order to be useful within other asynchronous scopes, the object must not have any blocking functions. In practice, this means the scope serves three purposes. It:

 * maintains state for launched work so that all in-flight senders have a well-defined location in which to store an `operation_state`
 * manages lifetimes for launched work so that in-flight tasks may be tracked, independent of any particular execution context
 * offers a join operation that may be used to continue more work, or block and wait for work, after some set of senders is complete, independent of the context on which they run.

This object would be used to spawn senders without waiting for each sender to complete.

The general concept of an async scope to manage work has been deployed broadly in [folly](https://github.com/facebook/folly/blob/main/folly/experimental/coro/AsyncScope.h) to safely launch awaitables in folly's [coroutine library](https://github.com/facebook/folly/tree/main/folly/experimental/coro) and in [libunifex](https://github.com/facebookexperimental/libunifex/blob/main/include/unifex/async_scope.hpp) where it is designed to be used with the sender/receiver pattern.

Async Scope
===========

The requirements for the async scope are:

 - An `async_scope` must be non-movable and non-copyable.
 - An `async_scope` must be empty when the destructor runs.
 - An `async_scope` must not provide any query CPO's on the receiver passed to the sender.
 - An `async_scope` must constrain `spawn()` to accept only senders that are never-blocking.
 - An `async_scope` must introduce a cancellation scope as well.
 - An `async_scope` must forward cancellation to all spawned senders.
 - An `async_scope` must provide a sender that completes when all spawned senders are complete.

Async scope defines an additional concept:

 - A `future_sender` that represents a potentially eagerly executing `sender`.


## Definitions

```cpp
struct async_scope {
    ~async_scope();
    async_scope(const async_scope&) = delete;
    async_scope(async_scope&&) = delete;
    async_scope& operator=(const async_scope&) = delete;
    async_scope& operator=(async_scope&&) = delete;

    auto spawn(sender) const&->void;
    auto spawn_future(sender) const&->future_sender<Values…>;

    auto empty() const&->empty_sender;

    auto get_stop_source() & ->stop_source&;
    auto get_stop_token() const& ->stop_token;
    auto request_stop() & ->void;
};


template<class... Ts>
struct future_sender; // see below;
```

## Lifetime

An `async_scope` object must outlive work that is spawned on it. It should be viewed as owning the storage for that work.
The `async_scope` may be constructed in a local context, matching syntactic scope or the lifetime of surrounding algorithms.
The destructor of the `async_scope` will terminate if there is outstanding work in the scope at destruction time, therefore `empty` must be called before the `async_scope` object is destroyed.

## spawn

`spawn(sender) const&->void;`

Eagerly launches work on the `async_scope`. 

When passed a valid `sender` `s` or type `S`, that satisfies `sender_of<S>` and that does not complete inline with the call to `start()` returns `void`. `s` is guaranteed to `start()`, if allocation of the `operation_state` succeeds. `s` is not required to `start()` before `spawn()` returns. 

## spawn_future

`spawn_future(sender) const&->future_sender<Values…>;`

Eagerly launches work on the `async_scope` but returns a `future_sender` that represents an eagerly running task.

When passed a valid `sender` `s` or type `S`, that satisfies `sender_of<S, Values...>` and that does not complete inline with the call to `start()` returns a `future_sender<Values...>`

It is safe to drop `f` without starting it because the `async_scope` safely manages the lifetime of the running work. `future_sender<>` `start()` is in a race with the completion of the sender, `s`, that has already been started. The race will be resolved by the `future_sender<>` state. 

Cancelling the `future_sender<Values...>`, `f`, cancels `s` and does not cancel the `async_scope`.

## empty detection

`empty() const&->empty_sender;`

empty_sender completes with void when all spawned senders have completed and no senders are in flight in the `async_scope`. An `async_scope` can be empty more than once. The intended usage is to spawn all the senders and then start the empty_sender to know when all spawned senders have completed. Another supported usage is to use request_stop on async_scope to prevent further senders from being spawned, and then start the empty_sender.

To safely destroy the `async_scope` object the `async_scope` must be stopped either through its `stop_source` via `get_stop_source()` or through a call to `request_stop()`, to prevent more work being spawned, then the `async_scope` must be left to drain.

That is to say the following is safe:

```cpp
{
  async_scope s;
  s.spawn(snd);
  s.request_stop();
  sync_wait(s.empty());
}
```

## stop

`get_stop_token() const& ->stop_token;`

Returns the `stop_token` associated with the `async_scope`. This will report stopped when the `stop_source` is stopped or `request_stop()` is called. The `stop_token` may be used by tasks launched on the `async_scope` to detect a stop request.


`get_stop_source() & ->stop_source;`

Returns a `stop_source` associated with the `async_scope`'s `stop_token`. This `stop_source` will trigger the `stop_token`, and will caused future calls to `spawn` and `spawn_future` to not start the passed sender. Future calls to `spawn_now` will silently drop the passed `sender`.

Calling `request_stop` on the returned `stop_source` will not cancel senders spawned on the `async_scope` except where the algorithms explicitly used the scope's `stop_source`.


`request_stop() & ->void;`

Stops the `async_scope` inline. Equivalent to calling `get_stop_source().request_stop()`.


Examples of use
===============
