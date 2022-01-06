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

Async scope defines three additional concepts:

 - An `async_storage_provider` that supports asynchronous memory allocation.
 - A `storage_sender` that is a sender of either `void` or a `future_sender`. The `storage_sender` completes when storage is allocated by the `async_storage_provider`.
 - A `future_sender` that represents a potentially eagerly executing `sender`.


## Definitions

```cpp
template<typename async_storage_provider>
struct async_scope {
    ~async_scope();
    explicit async_scope(async_storage_provider);

    async_scope(const async_scope&) = delete;
    async_scope(async_scope&&) = delete;
    async_scope& operator=(const async_scope&) = delete;
    async_scope& operator=(async_scope&&) = delete;

    auto spawn(sender) const&->storage_sender<>;
    auto spawn_now(sender) const&->void;
    auto spawn_future(sender) const&->storage_sender<future_sender<Values…>>;

    auto empty() const&->empty_sender;

    auto get_stop_source() & ->stop_source&;
    auto get_stop_token() const& ->stop_token;
    auto request_stop() & ->void;
};


template<class S, class... Ts>
concept storage_sender = sender_of<S, Ts...>;

struct async_storage_provider; // see below

template<class... Ts>
struct future_sender; // see below;
```

## Lifetime

An `async_scope` object must outlive work that is spawned on it. It should be viewed as owning the storage for that work.
The `async_scope` may be constructed in a local context, matching syntactic scope or the lifetime of surrounding algorithms.
The destructor of the `async_scope` will terminate if there is oustanding work in the scope at descrution time, therefore `empty` must be called before the `async_scope` object is destroyed.

## spawn

`spawn(sender) const&->storage_sender<>;`

Lazily launches work on the `async_scope` with support for limited storage.

When passed a valid `sender` `s` or type `S`, that satisfies `sender_of<S>` and that does not complete inline with the call to `start()` returns a `storage_sender<>` that completes with `void` when the result of `construct(s)` on the Async Scope's `async_storage_provider` completes and `s` has been started. `s` is guaranteed not to start until `start()` is called on the returned `storage_sender<>`. `s` is guaranteed to have started by the time the `storage_sender` completes successfully, and is guaranteed not to have been started if the `storage_sender` completes with `set_done` or `set_error`.

This allows bounded calls to spawn() to apply back-pressure. For example: `spawn(..).repeat_effect_until(..)` will only spawn the next sender once the previous sender has been started.

Cancelling the returned `storage_sender<>` only cancels `s`, if started, or the allocation if not. Cancelling the `storage_sender` does not cancel the entire `async_scope`.

## spawn_now

`spawn_now(sender) const&->void;`

Eagerly launches work on the `async_scope` while blocking until storage is available and the sender has been started (although not necessarily until the sender is actively running).

When passed a valid `sender` `s` or type `S`, that satisfies `sender_of<S>` and that does not compelete inline with the call to `start()`, `spawn_now(s)` is equivalent to `std::this_thread::sync_wait(spawn(s));`

## spawn_future

`spawn_future(sender) const&->storage_sender<future_sender<Values…>>;`

Lazily allocates capacity and launches work on the `async_scope` but returns a `future_sender` that represents an eagerly running task.

When passed a valid `sender` `s` or type `S`, that satisfies `sender_of<S, Values...>` and that does not complete inline with the call to `start()` returns a `storage_sender<future_sender<Values...>` that completes with a `future_sender<Values...>`, `f`, when the result of `construct(s)` on the Async Scope's `async_storage_provider` completes and `s` has been started. `s` is guaranteed not to start until `start()` is called on the returned `storage_sender<>`. `s` is guaranteed to have started by the time the `storage_sender` completes successfully, and is guaranteed not to have been started if the `storage_sender` completes with `set_done` or `set_error`.

This allows bounded calls to spawn() to apply back-pressure. For example: `spawn_future(..).repeat_effect_until(..)` will only spawn the next sender once the previous sender has been started.

`future_sender<Values...>` `f` completes when `s` completes, with the values passed by `s`. `s` will have been started by the time `f` is returned. It is safe to drop `f` without starting it because the `async_scope` safely manages the lifetime of the running work. `future_sender<>` `start()` is in a race with the completion of the sender that has already been started. The race will be resolved by the `future_sender<>` state. Storage for the `future_sender<>` state can be reserved in the original storage.

Cancelling the returned `storage_sender<future_sender<Values...>>` only cancels `s`, if started, or the allocation if not. Cancelling the `storage_sender` does not cancel the entire `async_scope`.
Cancelling the `future_sender<Values...>` passed on the value channel of the `storage_sender` cancels `s`.

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

Returns a `stop_source` associated with the `async_scope`'s `stop_token`. This `stop_source` will trigger the `stop_token`, and will caused future calls to `spawn` and `spawn_future` to not allocate capacity or start the passed sender and for the returned `storage_sender` to complete with `set_done`. Future calls to `spawn_now` will silently drop the passed `sender`.

Calling `request_stop` on the returned `stop_source` will not cancel senders spawned on the `async_scope` except where the algorithms explicitly used the scope's `stop_source`.


`request_stop() & ->void;`

Stops the `async_scope` inline. Equivalent to calling `get_stop_source().request_stop()`.


Async Storage
=============

The elephant in the room. What is AsyncStorage?

## What is async storage?

In resource constrained environments, it is important to limit resource consumption. Limits on consumption of memory, IO, compute, etc.. are all important. It turns out that all of these limits can be expressed in terms of async storage. IO needs storage for buffers, memory is storage and compute needs storage for `operation_state`.

Sometimes these limits are artificial. AsyncStorage can represent a fixed size storage that was reserved at compile-time. This allows storage without a runtime allocator.

Parameterizing an `async_scope` object on an AsyncStorage object allows these limits to be applied to the `async_scope`. This might be done to limit concurrency in that scope, or to eliminate allocations.

There are async storage objects that have no fixed limits. An async storage object that directly wraps a runtime allocator is a default that would add no overhead (the senders would be always-inline).

## How does async storage solve all these limits?

Limits in runtime allocators today are represented as an out of memory error. This is an expression of the intent for allocation failure to be rare and unexpected.

When an allocator has a limit that is expected and frequent, representing out of memory as an error is harmful. Such an allocator could decide to block until the allocation can be satisfied. This blocking behaviour is also harmful.

An async storage object will allow an implementation to represent out of memory as an error (this is useful for directly wrapping existing allocators), or as a non-blocking async operation that completes when the storage can be satisfied.

An object that has `get_storage_provider() const&` will return an object that can be used with `get_storage<T>()`. `get_storage<T>()` will return an object that cannot be copied or moved - because it may actually contain the limited storage for `T`. The object returned from `get_storage<T>()` supports `storage_sender construct(AN...)` - where `storage_sender` completes with a `storage-ref-T` - and `destruct_sender destruct(storage-ref-T)`

## Definitions

```cpp
template<typename T>
struct async_storage_ref {
    auto get() -> T&;
};
```

```cpp
template<typename T>
struct async_storage {
    ~async_storage();

    using ref_type = async_storage_ref<T>;

    async_storage(const async_storage&) = delete;
    async_storage(async_storage&&) = delete;
    async_storage& operator=(const async_storage&) = delete;
    async_storage& operator=(async_storage&&) = delete;

    template<typename... AN>
    auto construct(AN&&...) -> ref_type-sender;

    auto destruct(ref_type) -> destruct-sender;
};
```

```cpp
struct async_storage_provider {
    ~async_storage_provider();

    using any_ref_type = type-erased-ref;

    template<typename T>
    auto get_storage_for() -> async_storage<T>;
};
```

```cpp
struct context-provider {
    auto get_storage_provider() -> async_storage_provider;
};
```

Examples of use
===============
