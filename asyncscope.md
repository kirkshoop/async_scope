---
title: "`async_scope` -- Creating scopes for non-sequential concurrency"
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
  - name: Lucian Radu Teodorescu
    email: <lucteo@lucteo.ro>
toc: true
---

Changes
=======

## R0

- first revision

Introduction
============

A major precept of [@P2300R7] is structured concurrency. The `start_detached` and `ensure_started` algorithms are motivated by some important scenarios. Not every _`async-function`_ has a clear chain of work to consume or block on the result. The problem with these algorithms is that they provide unstructured concurrency. This is an unnecessary and unwelcome and undesirable property for concurrency. Using these algorithms leads to problems with lifetimes that are often 'fixed' using `shared_ptr` for ad-hoc garbage collection. 

This paper describes the `counting_scope` _`async-object`_. A `counting_scope` would be used to spawn many _`async-function`_ s safely inside an enclosing _`async-function`_. 

The _`async-function`_ s spawned with a `counting_scope` can be running on any execution context. The `counting_scope` _`async-object`_ has only one concern, which is to provide an _`async-object`_ that will destruct only after all the _`async-function`_ s spawned by the `counting_scope` have completed. 

## Implementation experience

The general concept of an async scope to manage work has been deployed broadly in Meta's folly [@asyncscopefolly] to safely launch awaitables in Folly's coro library [@corofolly] and in Facebook's libunifex library [@asyncscopeunifex] where it is designed to be used with the sender/receiver pattern.

## RAII and _`async-object`_ 

The definition of an _`async-object`_, and a description of how an _`async-object`_ attaches to an enclosing _`async-function`_, can be found in [@P2849R0]. An _`async-object`_ is attached to an enclosing _`async-function`_ in the same sense that a C++ object is attached to an enclosing C++ function. The enclosing _`async-function`_ always contains the construction use and destruction of all contained _`async-object`_ s.

The paper that describes how to build an _`async-object`_ was written when years of implementation experience with `async_scope` led to the discovery of a general pattern for all _`async-object`_ s, including thread-pools, sockets, files, allocators, etc..

Now the `counting_scope` is just the first _`async-object`_ proposed for standardization. The paper is greatly simplified by moving the material related to attaching a `counting_scope` to an enclosing _`async-function`_ to the [@P2849R0] paper.

Motivation
==========

## Motivating example

Let us assume the following code:

```c++
namespace ex = std::execution;

struct work_context;
struct work_item;
void do_work(work_context&, work_item*);
std::vector<work_item*> get_work_items();

int main() {
    static_thread_pool my_pool{8};
    work_context ctx; // create a global context for the application

    std::vector<work_item*> items = get_work_items();
    for ( auto item: items ) {
        // Spawn some work dynamically
        ex::sender auto snd = ex::transfer_just(my_pool.get_scheduler(), item)
                            | ex::then([&](work_item* item){ do_work(ctx, item); });
        ex::start_detached(std::move(snd));
    }
    // `ctx` and `my_pool` is destroyed
}
```

In this example we are creating parallel work based on the given input vector.
All the work will be spawned on the local `static_thread_pool` object, and will use a shared `work_context` object.

Because the number of work items is dynamic, one is forced to use `start_detached()` from [@P2300R7] (or something equivalent) to dynamically spawn work.
[@P2300R7] doesn't provide any facilities to spawn dynamic work and return a sender (i.e., something like `when_all` but with a dynamic number of input senders).

Using `start_detached()` here follows the _fire-and-forget_ style, meaning that we have no control over, or awareness of, the termination of the _`async-function`_ being spawned.

At the end of the function, we are destroying the `work_context` and the `static_thread_pool`.
But at that point, we don't know whether all the spawned _`async-function`_ s have completed.
If there are still _`async-function`_ s that are not yet complete, this might lead to crashes.

_NOTE:_ As described in [@P2849R0], the `work_context` and `static_thread_pool` objects need to be _`async-object`_ s because they are used by _`async-function`_ s.

[@P2300R7] doesn't give us out-of-the-box facilities to use in solving these types of problems.

This paper proposes the `counting_scope` facility that would help us avoid the invalid behavior.
With `counting_scope`, one might write safe code this way:
```c++
int main() {
    auto work = ex::use_resources( // NEW! @@_see_ P2849R0@@
      [](work_context ctx, static_thread_pool my_pool, counting_scope my_work_scope){
        std::vector<work_item*> items = get_work_items();
        for ( auto item: items ) {
            // Spawn some work dynamically
            ex::sender auto snd = ex::transfer_just(my_pool.get_scheduler(), item)
                                | ex::then([&](work_item* item){ do_work(ctx, item); });
            ex::spawn(my_work_scope, std::move(snd));            // MODIFIED!
        }
      }, 
      make_deferred<work_context_resource>(), // create a global context for the application
      make_deferred<static_thread_pool_resource>(8), // create a global thread pool 
      make_deferred<counting_scope_resource>()); // NEW!
    this_thread::sync_wait(work);   // NEW!
}
```

The newly introduced `counting_scope_resource` object allows us to attach the dynamic work we are spawning to the enclosing `use_resources` _see_ [@P2849R0]. This structure ensures that the `static_thread_pool` and `work_context` destruct after the spawned _`async-function`_ s complete.

Please see below for more examples.

## `counting_scope` is step forward towards Structured Concurrency

Structured Programming [@Dahl72] transformed the software world by making it easier to reason about the code, and build large software from simpler constructs.
We want to achieve the same effect on concurrent programming by ensuring that we _structure_ our concurrent code.
[@P2300R7] makes a big step in that direction, but, by itself, it doesn't fully realize the principles of Structured Programming.
More specifically, it doesn't always ensure that we can apply the _single entry, single exit point_ principle.

The `start_detached` sender algorithm fails this principle by behaving like a `GOTO` instruction.
By calling `start_detached` we essentially continue in two places: in the same function, and on different thread that executes the given work.
Moreover, the lifetime of the work started by `start_detached` cannot be bound to the local context.
This will prevent local reasoning, which will make the program harder to understand.

To properly structure our concurrency, we need an abstraction that ensures that all the _`async-function`_ s being spawned are attached to an enclosing _`async-function`_.
This is the goal of `counting_scope`.

## `counting_scope` may increase consensus for P2300 

Although [@P2300R7] is generally considered a strong improvement on concurrency in C++, various people voted against introducing this into the C++ standard.

This paper is intended to increase consensus for [@P2300R7].

Examples of use
===============

## Spawning work from within a task

Use a `counting_scope` in combination with a `system_context` from [@P2079R2] to spawn work from within a task and join it later:
```c++
using namespace std::execution;

int result = 0;

int main() {

  auto work = ex::use_resources( // NEW! @@_see_ P2849R0@@
    [&result](system_context ctx, counting_scope scope){
      scheduler auto sch = ctx.scheduler();

      sender auto val = on(
        sch, just() | then([&result, sch, scope](auto sched) {
            int val = 13;

            auto print_sender = just() | then([val]{
              std::cout << "Hello world! Have an int with value: " << val << "\n";
            });
            // spawn the print sender on sched to make sure it
            // completes before shutdown
            ex::spawn(scope, on(sch, std::move(print_sender)));

            return val;
          })
        ) | then([&result](auto val){result = val});

      ex::spawn(scope, std::move(val));
    }, 
    make_deferred<system_execution_resource>(8), // create a global thread pool 
    make_deferred<counting_scope_resource>()); // NEW!
  this_thread::sync_wait(work);   // NEW!

  std::cout << "Result: " << result << "\n";
}

// The counting scope ensured that all work is safely joined, so result contains 13
```

## Starting work nested within a framework
In this example we use the `counting_scope` within a class to start work when the object receives a message and to wait for that work to complete before closing.
`my_window::start()` starts the sender using storage reserved in `my_window` for this purpose.
```c++
using namespace std::execution;

// 
class my_window {
  //..

  // async-construction creates the 
  // async-object members
  system_context ctx;
  counting_scope scope{};

  scheduler auto sch{ctx.scheduler()};
};
class my_window_resource {
  //..
};

sender auto some_work(int id);

void my_window::onMyMessage(int i) {
  ex::spawn(this->scope, on(this->sch, some_work(i)));
}

void my_window::onClickClose() {
  this->post(close_message{});
}
```

## Starting parallel work

In this example we use the `counting_scope` within lexical scope to construct an algorithm that performs parallel work.
This uses the `let_value_with` [@letvwthunifex] algorithm implemented in [@libunifex] which simplifies in-place construction of a non-moveable object in the `let_value_with` algorithms' _`operation-state`_ object.
Here `foo` launches 100 tasks that concurrently run on some scheduler provided to `foo`, through its connected receiver, and then the tasks are asynchronously joined.
In this case the context the work is run on will be the `system_context`'s scheduler, from [@P2079R2].
This structure emulates how we might build a parallel algorithm where each `some_work` might be operating on a fragment of data.
```c++
using namespace std::execution;

sender auto some_work(int work_index);

sender auto foo(scheduler auto sch) {
  return ex::use_resources( // NEW! @@_see_ P2849R0@@
    [sch](counting_scope scope){
      return schedule(sch)
        | then([]{ std::cout << "Before tasks launch\n"; })
        | then(
          [sch, scope]{
            // Create parallel work
            for(int i = 0; i < 100; ++i)
              ex::spawn(scope, on(sch, some_work(i)));
            // Join the work with the help of the scope
          })
        ;
    },
    make_deferred<counting_scope_resource>()) // NEW!
    | then([]{ std::cout << "After tasks complete\n"; })
    ;
}
```

## Listener loop in an HTTP server

This example shows how one can write the listener loop in an HTTP server, with the help of coroutines.
The HTTP server will continuously accept new connection and start work to handle the requests coming on the new connections.
While the listening activity is bound in the scope of the loop, the lifetime of handling requests may exceed the scope of the loop.
We use `counting_scope` to limit the lifetime of the request handling without blocking the acceptance of new requests.

```c++
task<size_t> listener(int port, io_context& ctx, static_thread_pool& pool) {
  size_t count{0};
  // Continue only after all requests are handled
  co_await use_resources(// NEW! @@_see_ P2849R0@@
    [&count, ctx, pool](listening_socket listen_sock, counting_scope work_scope) -> task<> {
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
        work_ex::spawn(scope, std::move(snd));
      }
      co_return ;
    },
    make_deferred<listening_socket_resource>(port),
    make_deferred<counting_scope_resource>()); // NEW!
  // At this point, all the request handling is complete
  co_return count;
}
```

Async Scope, usage guide
========================

The requirements for the async scope are:

 - An _`async-scope`_ must allow an arbitrary sender to be nested within the scope without eagerly starting the sender (`nest()`).
 - An _`async-scope`_ must constrain `spawn()` to accept only senders that complete with `void`.
 - An _`async-scope`_ must start the given sender before `spawn()` and `spawn_future()` exit.

More on these items can be found below in the sections below.

## Definitions

```cpp
struct counting_scope {
  using self_t = counting_scope; /*@@_exposition-only_@@*/
  counting_scope();
  ~counting_scope();
  counting_scope(const self_t&) = delete;
  counting_scope(self_t&&) = delete;
  self_t& operator=(const self_t&) = delete;
  self_t& operator=(self_t&&) = delete;

  void
  /*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
    decays_to<self_t> auto&&, spawn_t, sender_to<@@_spawn-receiver_@@> auto&& s) noexcept;

  template <sender_to<@@_spawn-future-receiver_@@> S>
  @@_spawn-future-sender_@@<S>
  /*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
    decays_to<self_t> auto&&, spawn_future_t, S&& s) noexcept;

  template <sender S>
  @@_nest-sender_@@<S>
  /*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
    decays_to<self_t> auto&&, nest_t, S&& s) noexcept;
};
```

## Lifetime

The `counting_scope` keeps a counter of how many spawned _`async-function`_ s have not completed.

## `spawn()`

```c++
void
/*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
  decays_to<self_t> auto&&, spawn_t, sender_to<@@_spawn-receiver_@@> auto&& s) noexcept;
```

Eagerly launches work on the `counting_scope`.

This involves a dynamic allocation of the spawned sender's _`operation-state`_. The _`operation-state`_ is destroyed after the spawned sender completes.

This is similar to `start_detached()` from [@P2300R7], but the `counted_scope` keeps track of the spawned _`async-function`_ s.

The given sender must complete with `void` or `stopped`.
The given sender is not allowed to complete with an error; the user must explicitly handle the errors that might appear as part of the _`sender-expression`_ passed to `spawn()`.

As `spawn()` starts the given sender synchronously, it is important that the user provides non-blocking senders. 
This matches user expectations that `spawn()` is asynchronous and avoids surprising blocking behavior at runtime.
The reason for non-blocking start is that spawn must be non-blocking.
Using `spawn()` with a sender generated by `on(sched, @_blocking-sender_@)` is a very useful pattern in this context.

_NOTE:_ A query for non-blocking start will allow `spawn()` to be constrained to require non-blocking start.

Usage example:
```c++
...
for (int i=0; i<100; i++)
    spawn(s, on(sched, some_work(i)));
```


## `spawn_future()`

```c++
template <sender_to<@@_spawn-future-receiver_@@> S>
@@_spawn-future-sender_@@<S>
/*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
  decays_to<self_t> auto&&, spawn_future_t, S&& s) noexcept;
```

Eagerly launches work on the `counting_scope` and returns a _`spawn-future-sender`_ that provides access to the result of the spawned sender.

This involves a dynamic allocation of the _`spawn-future-sender`_ state, which includes the given sender `s` 's _`operation-state`_, and synchronization to resolve the race between the production of the given sender `s` 's result and the consumption of the given sender `s` 's result. The _`spawn-future-sender`_ state is destroyed after the given sender `s` completes and all copies of the  _`spawn-future-sender`_ have been destructed.

This is similar to `ensure_started()` from [@P2300R7], but the `counted_scope` keeps track of the spawned _`async-function`_ s.

Unlike `spawn()`, the sender given to `spawn_future()` is not constrained on a given shape.
It may send different types of values, and it can complete with errors.

It is safe to drop the sender returned from `spawn_future()` without starting it, because the `counting_scope` safely manages the destruction of the _`spawn-future-sender`_ state.

_NOTE:_ there is a race between the completion of the given sender and the start of the returned sender. The race will be resolved by the _`spawn-future-sender`_ state.

Cancelling the returned sender, cancels the given sender `s`, but does not cancel any other spawned sender.

If the given sender `s` completes with an error, but the returned sender is dropped, the error is dropped too.

Usage example:
```c++
...
sender auto snd = s.spawn_future(on(sched, key_work()))
                | then(continue_fun);
for ( int i=0; i<10; i++)
    spawn(s, on(sched, other_work(i)));
return when_all(s.on_empty(), std::move(snd));
```

## `nest()`

```c++
template <sender S>
@@_nest-sender_@@<S>
/*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
  decays_to<self_t> auto&&, nest_t, S&& s) noexcept;
```

Returns a _`nest-sender`_ that, when started, adds the given sender to the count of senders that the `counting_scope` object will require to complete before it will destruct.

A call to `nest()` does not start the given sender.
A call to `nest()` is not expected to incur allocations.

The sender returned by a call to `nest()` holds a reference to the `counting_scope` in order to add the given sender to the count of senders when it is started.
Connecting and starting the sender returned from `nest()` will connect and start the given sender and add the given sender to the count of senders that the `counting_scope` object will require to complete before it will destruct.

Similar to `spawn_future()`, `nest()` doesn't constrain the input sender to any specific shape.
Any type of sender is accepted.

Unlike `spawn_future()` the returned sender does not prevent the scope from ending.
It is safe to drop the returned sender without starting it. 
It is UB to start the returned sender after the `counting_scope` has been destroyed.

As `nest()` does not immediately start the given work, it is ok to pass in blocking senders.

One can say that `nest()` is more fundamental than `spawn()` and `spawn_future()` as the latter two can be implemented in terms of `nest()`.
In terms of performance, `nest()` does not introduce any penalty.
`spawn()` is more expensive than `nest()` as it needs to allocate memory for the operation.
`spawn_future()` is even more expensive than `spawn()`; the receiver needs to be type-erased and a possible race condition needs to be resolved.
`nest()` does not require allocations, so it can be used in a free-standing environment.

Cancelling the returned sender, once it is connected and started, cancels `s` but does not cancel the `counting_scope`.

Usage example:
```c++
...
sender auto snd = s.nest(key_work());
for ( int i=0; i<10; i++)
    spawn(s, on(sched, other_work(i)));
return on(sched, std::move(snd));
```

Design considerations
=====================

## Shape of the given sender

### Constraints on `set_value()`

It makes sense for `spawn_future()` and `nest()` to accept senders with any type of completion signatures.
The caller gets back a sender that can be chained with other senders, and it doesn't make sense to restrict the shape of this sender.

The same reasoning doesn't necessarily follow for `spawn()` as it returns `void` and the result of the spawned sender is dropped.
There are two main alternatives:

- do not constrain the shape of the input sender (i.e., dropping the results of the computation)
- constrain the shape of the input sender

The current proposal goes with the second alternative.
The main reason is to make it more difficult and explicit to silently drop result.
The caller can always transform the input sender before passing it to `spawn()` to drop the values manually.

> **Chosen:** `spawn()` accepts only senders that advertise `set_value()` (without any parameters) in the completion signatures.

### Handling errors in `spawn()`

The current proposal does not accept senders that can complete with error given to `spawn()`.
This will prevent accidental error scenarios that will terminate the application.
The user must deal with all possible errors before passing the sender to `counting_scope`.
I.e., error handling must be explicit.

Another alternative considered was to call `std::terminate()` when the sender completes with error.

Another alternative is to silently drop the errors when receiving them.
This is considered bad practice, as it will often lead to first spotting bugs in production.

> **Chosen:** `spawn()` accepts only senders that do not call `set_error()`. Explicit error handling is preferred over stopping the application, and over silently ignoring the error.

### Handling stop signals in `spawn()`

Similar to the error case, we have the alternative of allowing or forbidding `set_stopped()` as a completion signal.
Because the goal of `counting_scope` is to track the lifetime of the work started through it, it shouldn't matter whether that the work completed with success or by being stopped.
As it is assumed that sending the stop signal is the result of an explicit choice, it makes sense to allow senders that can terminate with `set_stopped()`.

The alternative would require transforming the sender before passing it to spawn, something like `s.spawn(std::move(snd) | let_stopped([]{ return just(); ))`.
This is considered boilerplate and not helpful, as the stopped scenarios should be implicit, and not require handling.

> **Chosen:** `spawn()` accepts senders that complete with `set_stopped()`.

### No shape restrictions for the senders passed to `spawn_future()` and `nest()`

Similarly to `spawn()`, we can constrain `spawn_future()` and `nest()` to accept only a limited set of senders.
But, because we can attach continuations for these senders, we would be limiting the functionality that can be expressed.
For example, the continuation can handle different types of values and errors.

> **Chosen:** `spawn_future()` and `nest()` accept senders with any completion signatures.

## P2300's `start_detached()`

The `spawn()` method in this paper can be used as a replacement for `start_detached` proposed in [@P2300R7].
Essentially it does the same thing, but it can also attach the spawned sender to the enclosing _`async-function`_.

## P2300's `ensure_started()`

The `spawn_future()` method in this paper can be used as a replacement for `ensure_started` proposed in [@P2300R7].
Essentially it does the same thing, but it can also attach the spawned sender to the enclosing _`async-function`_.

## Supporting the pipe operator

This paper doesn't support the pipe operator to be used in conjunction with `spawn()` and `spawn_future()`.
One might think that it is useful to write code like the following:

```c++
std::move(snd1) | spawn(s); // returns void
sender auto snd3 = std::move(snd2) | spawn_future(s) | then(...);
```

In [@P2300R7] sender consumers do not have support for the pipe operator.
As `spawn()` works similarly to `start_detached()` from [@P2300R7], which is a sender consumer, if we follow the same rationale, it makes sense not to support the pipe operator for `spawn()`.

On the other hand, `spawn_future()` is not a sender consumer, thus we might have considered adding pipe operator to it.
To keep consistency with `spawn()`, at this point the paper doesn't support pipe operator for `spawn_future()`.

If `spawn_future()` was an algorithm and the `spawn_future()` method was removed from `counting_scope`, then the pipe operator would be a natural and obvious fit.

Q & A
=====

## Why does the `counting_scope` after all nested and spawned sender complete?

- `stop_callback` is not a destructor because:
  - `request_stop()` is **asking** for early completion.
  - `request_stop()` does not end the lifetime of the operation, `set_value()`, `set_error()` and `set_stopped()` end the lifetime -- those are the destructors for an operation.
  - `request_stop()` might result in completion with `set_stopped()`, but `set_value()` and `set_error()` are equally valid.

`request_stop()` should not be called from a destructor because:
If a sync context intends to ask for early completion of an async operation, then it needs to wait for that operation to actually complete before continuing (`set_value()`, `set_error()` and `set_stopped()` are the destructors for the async operation), and sync destructors must not block.

Principles that discourage blocking in the destructor:

- Blocking must be explicit (exiting a sync scope is implicit -- and `shared_ptr` makes it even more scary as the destructor will potentially run at a different callstack and executino resource each time).
- Blocking must be grepable.
- Blocking must be rare.
- Blocking must be composable.
- Blocking is like `reinterpret_cast<>` -- the name should be long and scary.
- `join()` is grepable and explicit, it is not rare, it is not composable (There is a separate blocking wait for each object. One blocking wait for many different things to complete would be better)-- this is why _`async-resource`_ will attach to the enclosing _`async-function`_.

Every _`async-function`_ will join with non-blocking primitives and `sync_wait()` will be used to block some composition of those non-blocking primitives. The _`async-function`_ being stopped would complete before any _`async-resource`_ it is using completes -- without any blocking.

Naming
======

As is often true, naming is a difficult task.

## `counting_scope`

A `counting_scope` represents the root of a set of nested lifetimes.

One mental model for this is a semaphore. It tracks a count of lifetimes and fires an event when the count reaches 0.

Another mental model for this is block syntax. `{}` represents the root of a set of lifetimes of locals and temporaries and nested blocks.

Another mental model for this is a container. This is the least accurate model. This container is a value that does not contain values. This container contains a set of active senders (an active sender is not a value, it is an operation).

alternatives: `async_scope`

## `nest()`

This provides a way to build a sender that, when started, adds to the count of spawned and nested senders that `counting_scope` maintains. `nest()` does not allocate state, call connect or call start. `nest()` is the basis operation for `counting_scope`. `spawn()` and `spawn_future()` use `nest()` to add to the count that `counting_scope` maintains, and then they allocate, connect, and start the returned _`nest-sender`_.

It would be good for the name to indicate that it is a simple operation (insert, add, embed, extend might communicate allocation, which `nest()` does not do).

alternatives: `wrap()`

## `spawn()`

This provides a way to start a sender that produces `void` and add to the count that `counting_scope` maintains of nested and spwned senders. This allocates, connects and starts the given sender.

It would be good for the name to indicate that it is an expensive operation.

alternatives: `connect_and_start()`

## `spawn_future()`

This provides a way to start work and later ask for the result. This will allocate, connect, start and resolve the race (using synchronization primitives) between the completion of the given sender and the start of the returned sender. Since the type of the receiver supplied to the result sender is not known when the given sender starts, the receiver will be type-erased when it is connected.

It would be good for the name to be ugly, to indicate that it is a more expensive operation than `spawn()`.

alternatives: `spawn_with_result()`

Diagrams
========

```plantuml
!pragma layout smetana
title counting_scope classes

class counting_nest_impl<? satisfies operation> {
  + void start()
  - <<operation>>counting_impl* impl
  - <<receiver>>r
}

class counting_spawn_future_impl<? satisfies operation> {
  + void start()
  - <<operation>>counting_impl* impl
  - <<receiver>>r
}

class counting_nest<? satisfies sender> {
  + completion_signatures_of_t< <<sender>>s > \nget_completion_signatures(environment)
  + <<operation>>counting_nest_impl connect(<<receiver>>r)
}
counting_nest::connect -d-> counting_nest_impl 

class counting_spawn_future<? satisfies sender> {
  + completion_signatures_of_t< <<sender>>s > \nget_completion_signatures(environment)
  + <<operation>>counting_spawn_future_impl connect(<<receiver>>r)
}
counting_spawn_future::connect -d-> counting_spawn_future_impl 

class counting_scope<? satisfies async-scope> {
  + <<sender>>counting_nest nest(<<sender>>s)
  + void spawn(<<sender>>s)
  + <<sender>>counting_spawn_future spawn_future(<<sender>>s)
  - <<operation>>counting_impl* impl
}
counting_scope::nest -l-> counting_nest 
counting_scope::spawn_future -r-> counting_spawn_future 

class counting_item_impl<? satisfies operation> {
  + void start()
  - <<operation>>counting_impl* impl
  - <<receiver>>r
}

class counting_item<? satisfies sender> {
  + completion_signatures<set_value_t(counting_scope)> \nget_completion_signatures(environment)
  + <<operation>>counting_item_impl connect(<<receiver>>r)
}
counting_item::connect -u-> counting_item_impl 

class counting_impl<? satisfies operation> {
  + void start()
  - <<sequence-receiver>>r
  - int count
}
counting_scope::impl -d-* counting_impl
counting_item_impl::impl -u-* counting_impl
counting_nest_impl::impl -r-* counting_impl
counting_spawn_future_impl::impl -l-* counting_impl

class counting<? satisfies sequence-sender> {
  + completion_signatures<set_value_t(counting_scope)> \nget_completion_signatures(environment)
  + <<operation>>counting_impl sequence_connect(<<sequence-receiver>>r)
}
counting::sequence_connect -u-> counting_impl 

class counting_scope_resource<? satisfies async-resource> {
  + <<sequence-sender>>counting run()
}
counting_scope_resource::run -u-> counting 

```

```plantuml
!pragma layout smetana
title counting_scope nest() activity

start
:async-invoke nest(scope, func);

:nest(scope, func) [enter];

:scope ++count;

:async-invoke func;

:func [enter];

:func [result];

:nest(scope, func) [result];

:scope --count;

if (count) equals (0) then 
if (scope) is (unused) then 
:destruct scope;
else
->scope is in use;
endif
else
->count > 0;
endif
end
```

```plantuml
!pragma layout smetana
title counting_scope spawn() activity

start
:invoke spawn(scope, func);

:spawn(scope, func) [enter];

:allocate state for \n  connect(\n    nest(scope, func), spawn-receiver) ;

:async-invoke nest(scope, func);

:spawn(scope, func) [return void];

:nest(scope, func) [enter];

:nest(scope, func) [result];

end
```

```plantuml
!pragma layout smetana
title counting_scope spawn_future() activity

start
:invoke spawn-future = spawn_future(scope, func);

:spawn_future(scope, func) [enter];

:allocate state for \n  connect(\n    nest(scope, func), \n    spawn-future-receiver) ;

:async-invoke nest(scope, func);

:spawn_future(scope, func) [return spawn-future];

:nest(scope, func) [enter];

:nest(scope, func) [result];

:stored-result = result;

:async-invoke spawn-future;

:spawn-future [enter];

:spawn-future [stored-result];

end
```

Specification
=============

## Synopsis

```c++
namespace std::execution {

namespace { // @@_exposition-only_@@
    struct @@_spawn-receiver_@@ { // @@_exposition-only_@@
        friend void set_value(@@_spawn-receiver_@@) noexcept;
        friend void set_stopped(@@_spawn-receiver_@@) noexcept;
    };
    struct @@_run-sender_@@; // @@_exposition-only_@@
    template <typename S>
    struct @@_nest-sender_@@; // @@_exposition-only_@@
    template <typename S>
    struct @@_spawn-future-sender_@@; // @@_exposition-only_@@
}

struct counting_scope {
    counting_scope();
    ~counting_scope();
    counting_scope(const counting_scope&) = delete;
    counting_scope(counting_scope&&) = delete;
    counting_scope& operator=(const counting_scope&) = delete;
    counting_scope& operator=(counting_scope&&) = delete;

  void
  /*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
    decays_to<self_t> auto&&, spawn_t, sender_to<@@_spawn-receiver_@@> auto&& s) noexcept;

  template <sender_to<@@_spawn-future-receiver_@@> S>
  @@_spawn-future-sender_@@<S>
  /*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
    decays_to<self_t> auto&&, spawn_future_t, S&& s) noexcept;

  template <sender S>
  @@_nest-sender_@@<S>
  /*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
    decays_to<self_t> auto&&, nest_t, S&& s) noexcept;
};

struct counting_scope_resource {
  using self_t = counting_scope_resource;
  counting_scope_resource();
  ~counting_scope_resource();
  counting_scope_resource(const self_t&) = delete;
  counting_scope_resource(self_t&&) = delete;
  self_t& operator=(const self_t&) = delete;
  self_t& operator=(self_t&&) = delete;

  // Option A or Option B from P2849R0
  @@_run-sender_@@
  /*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
    decays_to<self_t> auto&&, run_t) noexcept;  
};

}
```

## `counting_scope::counting_scope`

1. `counting_scope::counting_scope` constructs the `counting_scope` object, in the empty state.
2. _Note_: It is always safe to call the destructor immediately after the constructor, without adding any work to the `counting_scope` object.

## `counting_scope::~counting_scope`

1. `counting_scope::~counting_scope` destructs the `counting_scope` object, freeing all resources

2. The destructor will call `terminate()` if there is outstanding work in the `counting_scope` object (i.e., work created by `nest()`, `spawn()` and `spawn_future()` did not complete).

3. _Note_: It is always safe to call the destructor after the sender returned by `on_empty()` sent the completion signal, provided that there were no calls to `nest()`, `spawn()` and `spawn_future()` since the _`on-empty-sender`_ was started.

## `counting_scope::spawn`

1. `counting_scope::spawn` is used to eagerly start a sender while keeping the execution in the lifetime of the `counting_scope` object.
2. _Effects_:
   - An _`operation-state`_ object `op` will be created by connecting the given sender to a receiver `recv` of type _`spawn-receiver`_.
   - If an exception occurs while trying to create `op` in its proper storage space, the exception will be passed to the caller.
   - If no exception is thrown while creating `op` and stop was not requested on our stop source, then:
     - `start(op)` is called (before `spawn()` returns).
     - The lifetime of `op` extends at least until `recv` is called with a completion notification.
   - `recv` supports the `get_stop_token()` query customization point object; this will return the stop token associated with `counting_scope` object.
   - The `counting_scope` will not be _empty_ until `recv` is notified about the completion of the given sender.

3. _Note_: the receiver will help the `counting_scope` object to keep track of how many operations are running at a given time.

## `counting_scope::spawn_future`

1. `counting_scope::spawn_future` is used to eagerly start a sender in the context of the `counting_scope` object, and returning a sender that will be triggered after the completion of the given sender.
    The lifetime of the returned sender is not associated with `counting_scope`.

2. The returned sender has the same completion signatures as the input sender.

3. _Effects_:
   - An _`operation-state`_ object `op` will be created by connecting the given sender to a receiver `recv`.
   - If an exception occurs while trying to create `op` in its proper storage space, the exception will be passed to the caller.
   - If no exception is thrown while creating `op` and stop was not requested on our stop source, then:
     - `start(op)` is called (before `spawn_future` returns).
     - The lifetime of `op` extends at least until `recv` is called with a completion notification.
     - If `rsnd` is the returned sender, then using it has the following effects:
       - Let `ext_op` be the _`operation-state`_ object returned by connecting `rsnd` to a receiver `ext_recv`.
       - If `ext_op` is started, the completion notifications received by `recv` will be forwarded to `ext_recv`, regardless whether the completion notification happened before starting `ext_op` or not.
       - It is safe not to connect `rsnd` or not to start `ext_op`.
     - The `counting_scope` will not be _empty_ until one of the following is true:
       - `rsnd` is destroyed without being connected
       - `rsnd` is connected but `ext_op` is destroyed without being started
       - If `rsnd` is connected to a receiver to return `ext_op`, `ext_op` is started, and `recv` is notified about the completion of the given sender
   - `recv` supports the `get_stop_token()` query customization point object; this will return a stop token object that will be stopped when:
     - the `counting_scope` object is stopped (i.e., by using `counting_scope::request_stop()`;
     - if `rsnd` supports  `get_stop_token()` query customization point object, when stop is requested to the object `get_stop_token(rsnd)`.

4. _Note_: the receiver `recv` will help the `counting_scope` object to keep track of how many operations are running at a given time.

5. _Note_: the type of completion signal that `op` will use does not influence the behavior of `counting_scope` (i.e., `counting_scope` object behaves the same way if the sender describes a work that ends with success, error or cancellation).

6. _Note_: cancelling the sender returned by this function will not have an effect about the `counting_scope` object.


## `counting_scope::nest`

1. `counting_scope::nest` is used to produce a _`nest-sender`_ that, when started, nests the sender within the lifetime of the `counting_scope` object.
   The given sender will be started when the _`nest-sender`_ is started.

2. The returned sender has the same completion signatures as the input sender.

3. _Effects_:
   - If `rsnd` is the returned _`nest-sender`_, then using it has the following effects:
     - Let `op` be the _`operation-state`_ object returned by connecting the given sender to a receiver `recv`.
     - Let `ext_op` be the _`operation-state`_ object returned by connecting `rsnd` to a receiver `ext_recv`.
     - Let `op` be stored in `ext_op`.
     - If `ext_op` is started, then `op` is started and the completion notifications received by `recv` will be forwarded to `ext_recv`.
     - _Note_: as `op` is stored in `ext_op`, calling `nest()` cannot start the given sender.
     - Once `rsnd` is connected and `ext_op` started the `counting_scope` will not be empty until `recv` is notified about the completion of the given sender.
   - `recv` supports the `get_stop_token()` query customization point object; this will return a stop token object that will be stopped when:
     - the `counting_scope` object is stopped (i.e., by using `counting_scope::request_stop()`;
     - if `rsnd` supports  `get_stop_token()` query customization point object, when stop is requested to the object `get_stop_token(rsnd)`.

4. _Note_: the type of completion signal that `op` will use does not influence the behavior of `counting_scope` (i.e., `counting_scope` object behaves the same way if the sender completes with success, error or cancellation).

5. _Note_: cancelling the sender returned by this function will not cancel the `counting_scope` object.

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
  - id: asyncscopefolly
    citation-label: asyncscopefolly
    type: header
    title: "folly::coro::async_scope"
    url: https://github.com/facebook/folly/blob/main/folly/experimental/coro/AsyncScope.h
    company: Meta Platforms, Inc
  - id: corofolly
    citation-label: corofolly
    type: repository
    title: "folly::coro"
    url: https://github.com/facebook/folly/tree/main/folly/experimental/coro
    company: Meta Platforms, Inc
  - id: asyncscopeunifex
    citation-label: asyncscopeunifex
    type: header
    title: "async_scope"
    url: https://github.com/facebookexperimental/libunifex/blob/main/include/unifex/async_scope.hpp
    company: Facebook, Inc
  - id: letvwthunifex
    citation-label: letvwthunifex
    type: documentation
    title: "let_value_with"
    url: https://github.com/facebookexperimental/libunifex/blob/main/doc/api_reference.md#let_value_withinvocable-state_factory-invocable-func---sender
    company: Facebook, Inc
  - id: libunifex
    citation-label: libunifex
    type: repository
    title: "libunifex"
    url: https://github.com/facebookexperimental/libunifex/
    company: Facebook, Inc
  - id: asyncscopestdexec
    citation-label: asyncscopestdexec
    type: repository
    title: "async_scope"
    url: https://github.com/NVIDIA/stdexec/blob/main/include/exec/async_scope.hpp
    company: NVIDIA Corporation
  - id: P2849R0
    citation-label: P2849R0
    type: paper
    title: "async-resource - aka async-RAII"
    author:
      - family: Shoop
        given: Kirk
      - family: Voutilainen
        given: Ville
    url: https://wg21.link/P2849R0


---
