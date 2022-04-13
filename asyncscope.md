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

A major precept of [@P2300R4] is structured concurrency. The `start_detached` and `ensure_started` algorithms are motivated by some important scenarios. Not every asynchronous operation has a clear chain of work to consume or block on the result. The problem with these algorithms is that they provide unstructured concurrency. This is an unnecessary and unwelcome and undesirable property for concurrency. It leads to problems with lifetimes, and it requires execution contexts to conflate task lifetime management with execution management.

This paper describes an object that would be used to create a scope that will contain all senders spawned within its lifetime. These senders can be running on any execution context. The scope object has only one concern, which is to contain the spawned senders to a lifetime that is nested within any other resources that they depend on. In order to be useful within other asynchronous scopes, the object must not have any blocking functions. In practice, this means the scope serves three purposes. It:

 * maintains state for launched work so that all in-flight senders have a well-defined location in which to store an `operation_state`
 * manages lifetimes for launched work so that in-flight tasks may be tracked, independent of any particular execution context
 * offers a join operation that may be used to continue more work, or block and wait for work, after some set of senders is complete, independent of the context on which they run.

This object would be used to spawn senders without waiting for each sender to complete.

The general concept of an async scope to manage work has been deployed broadly in [folly](https://github.com/facebook/folly/blob/main/folly/experimental/coro/AsyncScope.h) to safely launch awaitables in folly's [coroutine library](https://github.com/facebook/folly/tree/main/folly/experimental/coro) and in [libunifex](https://github.com/facebookexperimental/libunifex/blob/main/include/unifex/async_scope.hpp) where it is designed to be used with the sender/receiver pattern.

Motivation
==========

## Motivating example

Let us assume the following code:

```c++
namespace ex = std::execution;

struct work_context;
auto do_work(work_context, work_item*) -> void;

auto parallel_work(const std::vector<work_item*>& items) -> void {
    static_thread_pool my_pool{8};
    work_context ctx;
    for ( auto item: items ) {
        ex::sender auto snd = ex::transfer_just(my_pool.get_scheduler(), item)
                            | ex::then([&](work_item* item){ do_work(ctx, item); });
        ex::start_detached(std::move(snd));
    }
    // maybe more work here...
    // `ctx` and `my_pool` is destroyed
}
```

In this example we are creating parallel work based on the given input vector.
All the work will be spawned in the context of a local `static_thread_pool` object, and will use a shared `work_context` object.

Because the number of work items is dynamic, one is forced to use `start_detached()` from [@P2300R4] (or something equivalent) to dynamically spawn work.
[@P2300R4] doesn't provide any facilities to spawn dynamic work and return a sender (i.e., something like `when_all` but with a dynamic number of input senders).

Using `start_detached()` here follow the *fire-and-forget* style, meaning that we have no control over the termination of the work being started.
We don't have control over the lifetime of the operation being started.

At the end of the function, we are destroying the work context and the thread pool.
But at that point, we don't know whether all the operations have completed.
If there are still operations that are not yet complete, this might lead to crashes.

[@P2300R4] doesn't give us out-of-the-box facilities to use in solving these types of problems.

This paper proposes the `async_scope` facility that would help us avoid the invalid behavior.
With it, one might write a safe code this way:
```c++
auto parallel_work(const std::vector<work_item*>& items) -> void {
    static_thread_pool my_pool{8};
    work_context ctx;
    async_scope my_work_scope;                  // NEW!
    for ( auto item: items ) {
        ex::sender auto snd = ex::transfer_just(my_pool.get_scheduler(), item)
                            | ex::then([&](work_item* item){ do_work(ctx, item); });
        my_work_scope.spawn(std::move(snd));   // MODIFIED!
    }
    // maybe more work here...
    this_thread::sync_wait(my_work_scope.on_empty());      // NEW!
    // `ctx` and `my_pool` can now safely be destroyed
}
```

The newly introduced `async_scope` object allows us to control the lifetime of the dynamic work we are spawning.
We can wait for all the work that we spawn to be complete before we destruct the objects used by the parallel work.


## Step forward towards Structured Concurrency

Structured Programming [@Dahl72] transformed the software world by making it easier to reason about the code, and build large software from simpler constructs.
We want to achieve the same effect on concurrent programming by ensuring that we *structure* our concurrency code.
[@P2300R4] makes a big step in that direction, but, by itself, it doesn't fully realize the principles of Structured Programming.
More specifically, it doesn't always ensure that we can apply the *single entry, single exit point* principle. 

The `start_detached` sender algorithm fails this principle by behaving like a `GOTO` instruction.
By calling `start_detached` we essentially continue in two places: in the same function, and on different thread that executes the given work.
Moreover, the lifetime of the work started by `start_detached` cannot be bound to the local context.
This will prevent local reasoning, thus will make the program harder to understand.

To properly structure our concurrency, we need an abstraction that ensures that all the work being started has a proper lifetime guarantee.
This is the goal of `async_scope`.

## Intel criticism on P2300

Although [@P2300R4] is generally considered a strong improvement on concurrency in C++, Intel voted against introducing this into the C++ standard.
The main reason for this negative vote is the absence of a feature like `async_scope`.

This paper wants to fix this issue.

TODO: better formulation


Async Scope
===========

The requirements for the async scope are:

 - An `async_scope` must be non-movable and non-copyable.
 - An `async_scope` must be empty when the destructor runs.
 - An `async_scope` must introduce a cancellation scope.
 - An `async_scope` must not provide any query CPO's on the receiver passed to the sender, other than `get_stop_token()` (in order to forward cancellation of the async_scope `stop_source` to all nested and spawned senders).
 - An `async_scope` must allow an arbitrary sender to be nested within the scope without eagerly starting the sender (`nest()`).
 - An `async_scope` must constrain `spawn()` and `spawn_future()` to accept only senders that are never-blocking.
 - An `async_scope` must constrain `spawn()` to accept only senders that complete with void.
 - An `async_scope` must provide an `empty_sender` that completes when all spawned senders are complete.

## Definitions

```cpp
struct async_scope {
    ~async_scope();
    async_scope(const async_scope&) = delete;
    async_scope(async_scope&&) = delete;
    async_scope& operator=(const async_scope&) = delete;
    async_scope& operator=(async_scope&&) = delete;

    auto nest(sender) const&->nested_sender<Values…>;

    auto spawn(sender) const&->void;
    auto spawn_future(sender) const&->future_sender<Values…>;

    auto on_empty() const&->empty_sender;

    auto get_stop_source() & ->stop_source&;
    auto get_stop_token() const& ->stop_token;
    auto request_stop() & ->void;
};

template<class... Ts>
struct nested_sender; @_see below_@

template<class... Ts>
struct future_sender; @_see below_@

struct empty_sender; @_see below_@
```

## Lifetime

An `async_scope` object must outlive work that is spawned on it. It should be viewed as owning the storage for that work.
The `async_scope` may be constructed in a local context, matching the syntactic scope or the lifetime of surrounding algorithms.
The destructor of an `async_scope` will `terminate()` if there are outstanding senders in the scope at destruction time, therefore `on_empty` **must** be called and the returned `empty_sender` **must** have started and completed with `void`, before the `async_scope` object is destroyed.

## nest

`nest(sender) const&->nested_sender<Values…>;`

returns a `nested_sender` that extends the lifetime of the `async_scope` that produced it to include the lifetime of the nested_sender object and, if started, the lifetime of the given sender operation.

A call to `nest()` does not start the given sender.
A call to `nest()` is not expected to incur allocations.

When passed a valid `sender`, `s`, or type `S`, that satisfies `sender_of<S, Values...>` returns a `nested_sender<Values...>`. `s` will not be connected and started by the call to `nest()`.

Connecting and starting the `nested_sender<Values...>`, `n`, returned from `nest()` will connect and start `s`.

The object `n` prevents the scope from ending. It is safe to drop `n` without starting it and this will remove it from the `async_scope` lifetime. 

Cancelling the `nested_sender<Values...>`, `n`, once it is connected and started cancels `s` and does not cancel the `async_scope`.

## spawn

`spawn(sender) const&->void;`

Eagerly launches work on the `async_scope`. This involves an allocation for the `operation_state` of the sender until it completes.

When passed a valid `sender` `s` of type `S`, that satisfies `sender_of<S>` and that does not complete inline with the call to `start()` returns `void`.
`s` is guaranteed to `start()` if allocation of the `operation_state` succeeds.
`s` is not required to `start()` before `spawn()` returns.

## spawn_future

`spawn_future(sender) const&->future_sender<Values…>;`

Eagerly launches work on the `async_scope` but returns a `future_sender` that represents an eagerly running task. This involves an allocation for the `operation_state` of the sender, until it completes, and synchronization to resolve the race between the production of the result and the consumption of the result.

When passed a valid `sender` `s` or type `S`, that satisfies `sender_of<S, Values...>` and that does not complete inline with the call to `start()` returns a `future_sender<Values...>`

It is safe to drop the `future_sender<Values...>` `f` returned from `spawn_future()` without starting it because the `async_scope` safely manages the lifetime of the running work. `future_sender<>` `start()` is in a race with the completion of the sender, `s`, that has already been started. The race will be resolved by the `future_sender<>` state.

Cancelling the `future_sender<Values...>`, `f`, cancels `s` and does not cancel the `async_scope`.

## empty detection

`on_empty() const&->empty_sender;`

`empty_sender` completes with `void` when all spawned senders have completed and no senders are in flight in the `async_scope`.
An `async_scope` can be empty more than once.
The intended usage is to spawn all the senders and then start the `empty_sender` to know when all spawned senders have completed.

To safely destroy the `async_scope` object an empty_sender start must happen-after all calls to `nest()`, `spawn()` and `spawn_future()` are complete. Once empty_sender completes the async_scope can be safely destructed - or a new set of calls to `nest()`, `spawn()` and `spawn_future()` happen-before the start of a new empty_sender and the async_scope destructor happens-after the completion of the empty_sender.

That is to say the following is safe:

```cpp
{
  async_scope s;
  s.spawn(snd);
  sync_wait(s.on_empty());
}
```

## stop

`get_stop_token() const& ->stop_token;`

Returns the `stop_token` associated with the `async_scope`. This will report stopped when the `stop_source` is stopped or `request_stop()` is called. The `stop_token` is provided to all nested and spawned senders to respond to a stop request.

`get_stop_source() & ->stop_source;`

Returns a `stop_source` associated with the `async_scope`'s `stop_token`. This `stop_source` will trigger the `stop_token`, and will cause future calls to `nest()`, `spawn()` and `spawn_future()` to start with a `stop_token` that is already in the `stop_requested()` state.

Calling `request_stop` on the returned `stop_source` will forward that request to all the nested and spawned senders.

`request_stop() & ->void;`

Equivalent to calling `get_stop_source().request_stop()`.


Examples of use
===============

## Spawning work from within a task

Using a global `async_scope` in combination with a `system_context` from [@P2079R2] to spawn work from within a task and join it later:
```c++
using namespace std::execution;

system_context ctx;
int result = 0;

{
  async_scope scope;
  scheduler auto sch = ctx.scheduler();

  sender auto val = on(
    sch, just() | then([sch, &scope](auto sched) {

        int val = 13;

        auto print_sender = just() | then([val]{
          std::cout << "Hello world! Have an int with value: " << val << "\n";
        });
        // spawn the print sender on sched to make sure it
        // completes before shutdown
        scope.spawn(on(sch, std::move(print_sender)));

        return val;
    })
  ) | then([&result](auto val){result = val});

  scope.spawn(std::move(val));


  // Safely wait for all nested work
  this_thread::sync_wait(scope.on_empty());
};

// The scope ensured that all work is safely joined, so result contains 13
std::cout << "Result: " << result << "\n";

// and destruction of the context is now safe
```

## Starting parallel work

In this example we use the `async_scope` within lexical scope to construct an algorithm that performs parallel work.
This uses the [`let_value_with`](https://github.com/facebookexperimental/libunifex/blob/main/doc/api_reference.md#let_value_withinvocable-state_factory-invocable-func---sender) algorithm implemented in [libunifex](https://github.com/facebookexperimental/libunifex/) which simplifies in-place construction of a non-moveable object in the `let_value_with` algorithms operation state.
Here foo launches 100 tasks that concurrently run on some scheduler provided to `foo` through its connected receiver, and then asynchronously joined.
In this case the context the work is run on will be the `system_context`'s scheduler, from [@P2079R2].
This structure emulates how we might build a parallel algorithm where each `some_work` might be operating on a fragment of data.
```c++
using namespace std::execution;

auto foo(sender auto input) {
  auto l = let_value(
    when_all(
      std::move(input),
      get_scheduler()),
    [](auto val, auto sch) {
      return sch.schedule() |
        then([](){std::cout << "Before tasks launch\n";}) |
        let_value_with(
          [](){return async_scope{};},
          [&sch](async_scope& scope){
            for(int i = 0; i < 100; ++0) {
              scope.spawn(on(sch, some_work()));
            }
            return scope.on_empty() | then([](){std::cout << "After tasks complete\n";});
          });
    }
  )
}

void bar() {
  system_context ctx;
  // Provide the system context scheduler to start the work and propagate
  // through receiver queries
  this_thread::sync_wait(on(ctx.scheduler(), foo()));
}
```

## Listener loop in an HTTP server

This example shows how one can write the listener loop in an HTTP server, with the help of coroutines.
The HTTP server will continuously accept new connection and start work to handle the requests coming on the new connections.
While the listening activity is bound in the scope of the loop, the lifetime of handling requests may exceed the scope of the loop.
We use `async_scope` to limit the lifetime of handling the requests.

```c++
auto listener(int port, io_context& ctx, static_thread_pool& pool) -> task<size_t> {
    listening_socket listen_sock{port};
    async_scope work_scope;
    size_t count{0};
    while (!ctx.is_stopped()) {
        // Accept a new connection
        connection conn = co_await async_accept(ctx, listen_sock);
        count++;

        // Create work to handle the connection in the scope of `work_scope`
        conn_data data{std::move(conn), ctx, pool};
        sender auto snd
            = just() 
            | let_value([data = std::move(data)]() {
                  return handle_connection(data);
              })
            ;
        work_scope.spawn(std::move(snd));
    }
    // Wait until all requests are handled
    sync_wait(work_scope.on_empty());
    // At this point, all the request handling is complete 
    co_return count;
}
```


Design considerations
=====================

## shape of `async_scope`

### concept vs type

One option is to have a `async_scope` concept that has many implementations.

Another option is to have a type that has one implementation per library vender.

> **Chosen:** Due to time constraints, this paper proposes a type.

### one vs many

One option would be for `async_scope` to have:
  `nest(sender) -> nested_sender`
and not
  `spawn(sender) -> void`
  `spawn_future(sender) -> future_sender<..>`

This would remove questions of when and how the state is allocated and the operation started from the scope.

The single concern of the `async_scope` that only had `nest()` would be to combine the lifetimes of many senders within one async scope.

`spawn()` and `spawn_future()` would still exist, in some form, and would use an `async_scope` parameter or member or base class to place the sender within an `async_scope`.

Another option is to add `spawn()` and `spawn_future()` methods to `async_scope`.

> **Chosen:** Due to time constraints, this paper proposes to add methods for `spawn` and `spawn_future` in addition to `nest`.

### cpo vs method

One option is to define cpos for `nest`, `spawn`, `spawn_future` and `on_empty` that operate on anything that customizes those cpos. 

Another option is to define a type with `nest`, `spawn`, `spawn_future` and `on_empty` methods.

> **Chosen:** methods on a type.

## Constraints on `set_value`

It makes sense for `spawn_future()` to accept senders with any type of completion signatures.
The caller gets back a sender that can be used to be chained with other senders, and it doesn't make sense to restrict the shape of this sender.

The same reasoning doesn't necessarily follow for `spawn()` as the result of the spawned work is dropped.
There are two main alternatives:

- do not constrain the shape of the input sender (i.e., dropping the results of the computation)
- constrain the shape of the input sender

The current proposal goes with the second alternative.
The main reason for this is that it tries to prevent silent dropping of results.
The caller can always transform the input sender before passing it to `spawn()` doing the dropping of the values manually.

Thus, `spwan()` accepts only senders that advertize `set_value()` (without any parameters) in the completion signatures.

## Handling errors in `spawn`

The current proposal accepts senders that can complete with error.
On receiving the error, the implementation must call `std::terminate()`.

Another alternative is to not accept senders that can send errors.
This will prevent accidental errors scenarios that will terminate the application.

## Handling stop signals in `spawn`

Similar to the error case, we have the alternative of allowing or forbidding `set_stopped` as a completion signal.
Because the goal of `async_scope` is to track the lifetime of the work started through it, it shouldn't matter whether that the work completed with success or by being stopped.
As it is assumed that sending the stop signal is the result of an explicit choice, it makes sense to allow senders that can terminate with `set_stopped`.

The alternative would require transforming the sender before passing it to spawn, something like `s.spawn(std::move(snd) | let_stopped([]{ return just_stopped(); ))`.
This is considered boilerplate and not helpful, as the stopped scenarios should be explicit, and not exceptional.

## Stop handling

The paper requires that if the caller requests stop to an `async_scope` object, then this request is forwarded to the nested and spawned senders.

### `request_stop` on the `async_scope` is forwarded

When stop is requested to `async_scope`, then stop is also requested to operations that are not yet complete.
While this can be a good thing in many contexts, it is not the best strategy in all cases.

Consider an `async_scope` that is used to keep track of the work needed to handle requests.
When trying to gracefully shut down the application, one might need to drain the existing requests without stopping their processing. The way to do that is to use the `empty_sender` without stopping the `async_scope`.

Consider `spawn()` in isolation. Forwarding the cancellation of the `async_scope` to the spawned senders would be natural.

Consider `nest()` and `spawn_future()`. They must combine two potential stop tokens. One from the `async_scope` and the other from the receiver passed to the returned `nested_sender` and `future_sender`. The semantics would be that either stop token would cancel the sender and would not stop the `async_scope`s `stop_source`.

Consider the use case where a reference to an `async_scope` is provided to many nested operations and functions to attach senders that they produce. Some of those senders may restore an invariant in a file-system or some other system. The way for a nested operation and function to make sure that the invariant is not corrupted by a forwarded stop request from the `async_scope`, is to apply a `never_stoppable_token` to their sender to hide the token provided by the `async_scope`.

### `request_stop` on the `async_scope` is not forwarded

A motivation for not forwarding a stop request was that a `stop_callback` is not a destructor, it is a signal requesting running work to stop. If `request_stop()` was called within the `async_scope` destructor, or any other destructor, then those destructors would be expected to block until an `empty_sender` completed. As falling off a scope or having a shared_ptr count reach 0 is implicit, it is very difficult to ensure that a `request_stop()` followed by starting an `empty_sender` would not have a race with concurrent calls to `nest()`, `spawn()` and `spawn_future()`. 

### inverting the forwarding default

Either of the two cases can be simulated with the help of the other case.

Example: When cancellation is not forwarded and forwarding is wanted, inject the same `stop_token` into all the spawned senders that need to be cancelled. 

Example: When cancellation is forwarded and forwarding is not wanted, mask the receiver provided `stop_token` by injecting a `never_stoppable_token` into all the spawned senders that need to complete even when cancelled. 

## Uses in other concurrent abstractions

We anticipate the interface `async_scope` to be used in other concurrent abstractions.
This implies that it is useful to think of this interface in a larger context.
If the interface is fit for the other purposes, it may be an indication that we have the right interface.

Let us consider a concurrent abstraction that will serialize dynamic work provided to it.
That is, if try to start multiple operations at the same time, only one is executed at a given time; the other ones are queued and will be executed whenever the previous operations complete. 

An interface to this abstraction might look like the following:
```c++
struct serializer {
    ~serializer();
    serializer(const serializer&) = delete;
    serializer(serializer&&) = delete;
    serializer& operator=(const serializer&) = delete;
    serializer& operator=(serializer&&) = delete;

    auto spawn(sender auto&& snd) -> void;
    auto spawn_future(sender auto&& snd) -> future_sender<S>;

    [[nodiscard]]
    auto on_empty() const noexcept -> empty_sender;
    
    auto get_stop_source() noexcept -> in_place_stop_source&;
    auto get_stop_token() const noexcept -> in_place_stop_token;
    auto request_stop() noexcept -> void;
};
```

One can add work in the context of the serializer.
One might want to add some work that needs to be executed in the serializer, then continue with some other work outside the serializer.
One might want to wait until the serializer is drained, or might want to stop processing any work in the serializer.
All of these can be fulfilled by this interface, and this is the same interface as `async_scope`.

Similar to this abstraction, one might imagine abstractions that can execute maximum *N* concurrent work items, or abstractions that execute work based on given labels, or abstractions that execute work based on dynamic priorities, etc.
All of these can be obtained by using an interface similar to the one we have for `async_scope`, maybe with some extra arguments.  

This provides a strong indication that the API for `async_scope` is appropriate.

## Removal of P2300's `start_detached`

The `async_scope::spawn` method can be used as a replacement for `start_detached` proposed in [@P2300R4].
Essentially it does the same thing, but it can also control the lifetime of the spawned work.

This paper might propose the removal of `start_detached` from `std::execution`. However, at this point, the paper doesn't make this proposal.

## Supporting pipe operator

The paper, as expressed doesn't support the pipe operator to be used in conjunction with `spawn()` and `spawn_future()`.
One might think that it is useful to write code like the following:

```c++
async_scope s;
std::move(snd1) | s.spawn(); // returns void
sender auto s = std::move(snd2) | s.spawn_future() | then(...);
```

In [@P2300R4] senders consumers won't have support for pipe operator.
As `spawn()` works similar to `start_detached` from [@P2300R4], which is a sender consumer, if we follow the same rationale, it makes sense not to support pipe operator for `spawn()`.

On the other hand, `spawn_future()` is not a sender consumer, thus we might have considered adding pipe operator to it.
To keep consistency with `spawn()`, at this point the paper doesn't support pipe operator for `spawn_future()`.

##  Use of customization point objects
Unlike [@P2300R4], this paper does not propose any use of customization point objects.
While `start_detached` is a customization point object, its correspondent, `async_scope::spawn` is not.

At this point, we do not believe that customizing the operations of `async_scope` will be needed by the users.
Therefore, the paper does not propose any customization point objects.

Q & A
=====

## Q why does async_scope terminate in the destructor instead of blocking like jthread?

One author's perspective:

`jthread` blocking in the destructor is bad for composition. 
`jthread` and `thread` should `terminate()` if the destructor runs before the thread exits. 

Imagine `make_shared<jthread>(..)`. 
Where will the destructor run? 
In what context will the destructor run? 

We can require users to know whether the destructor blocks for every type, and require users to carefully control the lifetime of all those objects - with the only indication of failure being a deadlock. Or we can teach that destructors will not block and indicate lifetime failures with `terminate()`. 

software is less likely to ship with crashes and more likely to be fixed when there are crashes. Deadlocks result in users forcefully terminating the app and forced terminations are rarely reported to the developer as a bug and even if reported, tend to have no debug data (stacks, dumps, etc..). If there is a lifetime bug that you want fixed - it had better crash.

Blocking must be explicit (exiting a sync scope is implicit - and shared_ptr makes it even more scary). 
Blocking must be grepable. 
Blocking must be rare. 
Blocking is like reinterpret_cast<> - the name should be long and scary. 
`join()` is a terrible design - this is why async_scope has `on_empty()` instead. 
Every asychronous lifetime must be managed with non-blocking primitives and only `sync_wait()` is used to block.

## Q why doesn't the async_scope destructor stop all the nested and spawned senders?

One author's perspective:

`stop_callback` is not a destructor because:
`request_stop()` is **asking** for early completion. 
`request_stop()` does not end the lifetime of the operation, `set_value()`, `set_error()` and `set_stopped()` end the lifetime - those are the destructors for an operation.
`request_stop()` might result in completion with `set_stopped()`, but `set_value()` and `set_error()` are equally likely. 

`request_stop()` should not be called from a destructor because:
If a sync context intends to ask for early completion of an async operation, then it needs to wait for that operation to actually complete before continuing (`set_value()`, `set_error()` and `set_stopped()` are the destructors for the async operation), and sync destructors must not block. see [Q why does async_scope terminate in the destructor instead of blocking like jthread?]. 

> NOTE: async RAII, could be used to signal early completion because it would be composed with other async operation lifetimes. The operation being stopped would complete before the aync RAII operation completed - without any blocking.

Naming
======

As is often true, naming is a difficult task.

## `async_scope`

This represents the root of a set of nested lifetimes.

One mental model for this is a semaphore. It tracks a count of lifetimes and fires an event when the count reaches 0.

Another mental model for this is block syntax. `{}` represents the root of a set of lifetimes of locals and temporaries and nested blocks.

Another mental model for this is a container. This is the least accurate model. This container is a value that does not contain values. This container contains a set of lifetimes that have not completed (a lifetime is not a value).

alternatives: `sender_scope`, `dynamic_scope`, `dynamic_lifetime`, `scope`, `lifetime`

## `nest`

This provides a way to extend the lifetime to include a sender. This does not allocate state, call connect or start. This is the basic operation for `async_scope`. `spawn` and `spawn_future` use `nest` to extend the scope and then allocate, connect and start.

It would be good for the name to indicate that it is a simple operation (insert, add, embed, extend might communicate allocation, which this does not do).

alternatives: `add`, `extend`, `embed`, `include`

## `spawn`

This provides a way to start a sender that produces `void` and extend the lifetime of the `async_scope` to exceed the lifetime of the operation. This allocates, connects and starts the sender.

It would be good for the name to indicate that it is an expensive operation.

alternatives: `start`, `submit`, `enqueue`, `do`, `run`

## `spawn_future`

This provides a way to start work and later ask for the result. This will allocate, connect, start and resolve the race (using synchronization primitives) between the completion of the given sender and the start of the returned sender. Since the type of the receiver supplied to the result sender is not known when the given sender starts, the receiver will be type-erased when it is connected.

It would be good for this name to be ugly to indicate that it is very expensive.

alternatives: `spawn_continue`, `spawn_result`, `spawn_with_result`, `spawn_buffered`, `spawn_virtual`, `spawn_dynamic`

> _Note_: "`spawn`" in these alternatives would be replaced by the alternative selected for `spawn`

## `on_empty`

This provides a way to get a sender that completes when the all the lifetimes nested inside the `async_scope` complete.

`empty` falls out of the poor mental model of `async_scope` being a container. `ended`, `complete` etc.. are problematic because additional senders might be used to extend the lifetime after the sender returned has completed.

This is the async version of a 'get' member function. A pattern was established a long time ago to not prefix 'get' methods on an object in std with `get_`. What is the current guidance? Do we want a prefix for async queries on objects in std?

alternatives: `empty`, `ready`, `when_empty`, `when_ready`, `on_ready`

## table of how some alternatives might be combined

+----+-----------------------+---------+------------+------------------+--------------+
| id | comments              | nest    | spawn void | spawn w/result   | empty        |
+:==:+:======================+:========+:===========+:=================+:=============+
| a: | status quo            | `nest`  | `spawn`    | `spawn_future`   | `on_empty`   |
+----+-----------------------+---------+------------+------------------+--------------+
| b: | removes confusion     | `add`   | `spawn`    | `spawn_continue` | `when_empty` |
|    | around “future”,      |         |            |                  |              | 
|    | “empty” and "nest"    |         |            |                  |              |
+----+-----------------------+---------+------------+------------------+--------------+
| c: |  tries to match       | `add`   | `start`    | `start_continue` | `when_empty` |
|    | the `start_detached`  |         |            |                  |              |
+----+-----------------------+---------+------------+------------------+--------------+
| d: | tries an alternative  | `add`   | `start`    | `start_chain`    | `when_empty` |
|    | to using “continue”   |         |            |                  |              |
+----+-----------------------+---------+------------+------------------+--------------+
| e: | tries an alternative  | `extend`| `start`    | `start_result`   | `on_ready`   |
|    | “result” and "extend" |         |            |                  |              |
|    | and "ready"           |         |            |                  |              |
+----+-----------------------+---------+------------+------------------+--------------+


Specification
=============

## Synopsis

```c++
namespace std::execution {

namespace { @_exposition-only_@ 
    struct async_scope_receiver @_exposition-only_@
        friend auto set_value(async_scope_receiver) noexcept -> void;
        template <typename E>
        friend auto set_error(async_scope_receiver, E) noexcept -> void;
        friend auto set_stopped(async_scope_receiver) noexcept -> void;
    };
    template <typename S>
    struct nested_sender; @_exposition-only_@
    template <typename S>
    struct future_sender; @_exposition-only_@
    struct empty_sender { @_exposition-only_@
        using completion_signatures =
            execution::completion_signatures<
                execution::set_value_t()
            >;
    };
}

struct async_scope {
    ~async_scope();
    async_scope(const async_scope&) = delete;
    async_scope(async_scope&&) = delete;
    async_scope& operator=(const async_scope&) = delete;
    async_scope& operator=(async_scope&&) = delete;

    template <sender S>
    auto nest(S&& snd) -> nested_sender<S>;

    template <sender_to<async_scope_receiver> S>
    auto spawn(S&& snd) -> void;
    template <sender S>
    auto spawn_future(S&& snd) -> future_sender<S>;

    [[nodiscard]]
    auto on_empty() const noexcept -> empty_sender;
    
    auto get_stop_source() noexcept -> in_place_stop_source&;
    auto get_stop_token() const noexcept -> in_place_stop_token;
    auto request_stop() noexcept -> void;
};

}
```

## `async_scope::~async_scope`

1. `async_scope::~async_scope` destructs the `async_scope` object, freeing all resources

2. The destructor will call `terminate()` if there is outstanding work in the `async_scope` object (i.e., work created by `spawn()` and `spawn_future()` did not complete).

3. *Note*: It is always safe to call the destructor after the sender returned by `on_empty()` sent the completion signal, provided that there were no calls to `nest()`, `spawn()` and `spawn_future()` since the `empty_sender` was started.

## `async_scope::nest`

1. `async_scope::nest` is used to produce a `nested_sender` that nests the sender within the lifetime of the `async_scope` object. The given sender will be started when the `nested_sender` is started.

2. The returned sender has the same completion signatures as the input sender.

3. *Effects*:
  - The `async_scope` will not be `empty` 
    - until the returned `nested_sender` is destroyed 
    - or the returned `nested_sender` is started and the operation completes
  - If `rsnd` is the returned `nested_sender`, then using it has the following effects:
    - Let `op` be the `operation_state` object returned by connecting the given sender to a receiver `recv`.
    - Let `ext_op` be the `operation_state` object returned by connecting `rsnd` to a receiver `ext_recv`.
    - Let `op` be stored in `ext_op`.
    - If `ext_op` is started, then `op` is started and the completion notifications received by `recv` will be forwarded to `ext_recv`.
    - It is safe not to connect `rsnd` or not to start `ext_op`.

4. *Note*: the receiver `recv` will help the `async_scope` object to keep track of how many operations are running at a given time.

5. *Note*: the type of completion signal that `op` will use does not influence the behavior of `async_scope` (i.e., `async_scope` object behaves the same way if the sender describes a work that ends with success, error or cancellation).  

6. *Note*: cancelling the sender returned by this function will not have an effect about the `async_scope` object.  

## `async_scope::spawn`

1. `async_scope::spawn` is used to eagerly start a sender while keeping the execution in the lifetime of the `async_scope` object.
2. *Effects*:
   - An `operation_state` object `op` will be created by connecting the given sender to a receiver `recv` of type `async_scope_receiver`.
   - If an exception occurs will be trying to create `op` in its proper storage space, the exception will be passed to the caller.
   - If no exception is thrown while creating `op` and stop was not requested on our stop source, then:
     - `start(op)` is called.
     - The lifetime of `op` extends at least until `recv` is called with a completion notification.
     - If the given work calls `set_error()` on `recv` then `std::terminate()` will be called.
3. *Note*: the receiver will help the `async_scope` object to keep track of how many operations are running at a given time. 

## `async_scope::spawn_future`

1. `async_scope::spawn_future` is used to eagerly start a sender in the context of the `async_scope` object, and returning a sender that will be triggered after the completion of the given sender.
    The lifetime of the returned sender is not associated with `async_scope`.

2. The returned sender has the same completion signatures as the input sender.

3. *Effects*:
   - An `operation_state` object `op` will be created by connecting the given sender to a receiver `recv`.
   - If an exception occurs will be trying to create `op` in its proper storage space, the exception will be passed to the caller.
   - If no exception is thrown while creating `op` and stop was not requested on our stop source, then:
     - `start(op)` is called.
     - The lifetime of `op` extends at least until `recv` is called with a completion notification.
     - If `rsnd` is the returned sender, then using it has the following effects:
       - Let `ext_op` be the `operation_state` object returned by connecting `rsnd` to a receiver `ext_recv`.
       - If `ext_op` is started, the completion notifications received by `recv` will be forwarded to `ext_recv`, regardless whether the completion notification happened before starting `ext_op` or not.
       - It is safe not to connect `rsnd` or not to start `ext_op`.

4. *Note*: the receiver `recv` will help the `async_scope` object to keep track of how many operations are running at a given time.

5. *Note*: the type of completion signal that `op` will use does not influence the behavior of `async_scope` (i.e., `async_scope` object behaves the same way if the sender describes a work that ends with success, error or cancellation).  

6. *Note*: cancelling the sender returned by this function will not have an effect about the `async_scope` object.  


## `async_scope::on_empty`

1. `async_scope::on_empty` returns a sender that can be used to get notifications when all the work belonging to the `async_scope` object is completed.

2. *Effects*:
   - Let `rsnd` be the sender returned by this function
   - Let `ext_op` be the `operation_state` object returned by connecting `rsnd` to a receiver `ext_recv`.
   - If `ext_op` is started, then `ext_recv` will be notified with `set_value()` whenever all the work started in the context of the `async_scope` object (by using `nest()`, `spawn` and `spawn_future`) is completed, and no senders are in flight.
   - It is safe not to connect `rsnd` or not to start `ext_op`.

3. *Note*: it is safe to call `on_empty()` multiple times on the same object and use the returned sender; it is also safe to use the returned senders in parallel.

4. *Note*: it is safe to call `on_empty()` and use the returned sender in parallel to calling `nest()`, `spawn()` and `spawn_future()` on the same `async_scope` object.

5. *Note*: it is not safe to destruct the `async_scope` when the sender returned from a call to `on_empty()` completes after being used in parallel to calling `nest()`, `spawn()` and `spawn_future()` on the same `async_scope` object.

## `async_scope::get_stop_source`

1. Returns an `in_place_stop_source` object associated with `async_scope`.

2. Requesting stop on the returned stop source will ensure that work added to the `async_scope` object by using `nest()`, `spawn()` and `spawn_future()` is given a stop token that already has `stop_requested() == true`.

3. *Note*: requesting stop on the returned stop source will automatically request to stop all the work that has been nested and spawned in the `async_scope` object with the help of `nest()`, `spawn()` and `spawn_future()`.

## `async_scope::get_stop_token`

1. This is equivalent to calling `get_stop_source().get_token()`.

## `async_scope::request_stop`

1. This is equivalent to calling `get_stop_source().request_stop()`.

TODO (LucTeo): This is currently different from the implementation

---
references:
  - id: Dahl72
    citation-label: Dahl72
    type: book
    title: "Structured Programming"
    author:
      - family: Dahl
        given: O.-J.
      - family: Dijkstra
        given: E. W.
      - family: Hoare
        given: C. A. R.
    publisher: Academic Press Ltd., 1972
---