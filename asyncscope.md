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
    ex::sync_wait(my_work_scope.empty());      // NEW!
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
The destructor of the `async_scope` will terminate if there is outstanding work in the scope at destruction time, therefore `empty` **must** be called before the `async_scope` object is destroyed.

## spawn

`spawn(sender) const&->void;`

Eagerly launches work on the `async_scope`.

When passed a valid `sender` `s` of type `S`, that satisfies `sender_of<S>` and that does not complete inline with the call to `start()` returns `void`.
`s` is guaranteed to `start()` if allocation of the `operation_state` succeeds.
`s` is not required to `start()` before `spawn()` returns.

## spawn_future

`spawn_future(sender) const&->future_sender<Values…>;`

Eagerly launches work on the `async_scope` but returns a `future_sender` that represents an eagerly running task.

When passed a valid `sender` `s` or type `S`, that satisfies `sender_of<S, Values...>` and that does not complete inline with the call to `start()` returns a `future_sender<Values...>`

It is safe to drop `f` without starting it because the `async_scope` safely manages the lifetime of the running work. `future_sender<>` `start()` is in a race with the completion of the sender, `s`, that has already been started. The race will be resolved by the `future_sender<>` state.

Cancelling the `future_sender<Values...>`, `f`, cancels `s` and does not cancel the `async_scope`.

## empty detection

`empty() const&->empty_sender;`

`empty_sender` completes with `void` when all spawned senders have completed and no senders are in flight in the `async_scope`.
An `async_scope` can be empty more than once.
The intended usage is to spawn all the senders and then start the empty_sender to know when all spawned senders have completed.
Another supported usage is to use `request_stop` on async_scope to prevent further senders from being spawned, and then start the `empty_sender`.

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

Calling `request_stop` on the returned `stop_source` will not cancel senders spawned on the `async_scope` except where the algorithms explicitly used the scope's `stop_token`.


`request_stop() & ->void;`

Stops the `async_scope` inline. Equivalent to calling `get_stop_source().request_stop()`.


Examples of use
===============

Using a global `async_scope` in combination with a `system_context` from [@P2079R2] so spawn work from within a task and join it later:
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
  this_thread::sync_wait(scope.empty());
};

// The scope ensured that all work is safely joined, so result contains 13
std::cout << "Result: " << result << "\n";

// and destruction of the context is now safe
```

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
            return scope.empty() | then([](){std::cout << "After tasks complete\n";});
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

Specification
=============

## Synopsis

```c++
namespace std::execution {

namespace { // exposition only 
    struct async_scope_receiver { // exposition only
        friend auto set_value(async_scope_receiver) noexcept -> void;
        template <typename E>
        friend auto set_error(async_scope_receiver, E) noexcept -> void;
        friend auto set_stopped(async_scope_receiver) noexcept -> void;
    };
    template <typename S>
    struct future_sender; // exposition only
    struct empty_sender { // exposition only
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

    template <sender_to<async_scope_receiver> S>
    auto spawn(S&& snd) -> void;
    template <sender_to<async_scope_receiver> S>
    auto spawn_future(S&& snd) -> future_sender<S>;

    [[nodiscard]]
    auto empty() const noexcept -> empty_sender;
    
    auto get_stop_source() noexcept -> in_place_stop_source&;
    auto get_stop_token() const noexcept -> in_place_stop_token;
    auto request_stop() noexcept -> void;
};

}
```

## `async_scope::~async_scope`

1. `async_scope::~async_scope` destructs the `async_scope` object, freeing all resources

2. The destructor will call `terminate()` if there is outstanding work in the `async_scope` object (i.e., work created by `spawn()` and `spawn_future()` did not complete).

3. *Note*: It is always safe to call the destructor after the sender returned by `empty()` sent the completion signal, provided that there were no calls to `spawn()` and `spawn_future()` since `empty()` was called, or stop was requested on our stop source before any of these `spawn()` and `spawn_future()` calls.


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



## `async_scope::empty`

1. `async_scope::empty` returns a sender that can be used to get notifications when all the work belonging to the `async_scope` object is completed.

2. *Effects*:
   - Let `rsnd` be the sender returned by this function
   - Let `ext_op` be the `operation_state` object returned by connecting `rsnd` to a receiver `ext_recv`.
   - If `ext_op` is started, then `ext_recv` will be notified with `set_value()` whenever all the work started in the context of the `async_scope` object (by using `spawn` and `spawn_future`) is completed, and no senders are in flight.
   - It is safe not to connect `rsnd` or not to start `ext_op`.

3. *Note*: it is safe to call `empty()` multiple times on the same object and use the returned sender; it is also safe to use the returned senders in parallel.

4. *Note*: it is safe to call `empty()` and use the returned sender in parallel to calling `spawn()` and `spawn_future()` on the same `async_scope` object.  


## `async_scope::get_stop_source`

1. Returns an `in_place_stop_source` object associated with `async_scope`.

2. Requesting stop on the returned stop source will ensure that no work can be added to the `async_scope` object by using `spawn()` and `spawn_future()`.

3. *Note*: requesting stop on the returned stop source doesn't automatically request stop to all the work that has been spawned in the `async_scope` object with the help of `spawn()` and `spawn_future()`.

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