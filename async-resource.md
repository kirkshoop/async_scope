---
title: "async-resource - aka async-RAII"
subtitle: "Draft Proposal"
document: D2849R0
date: today
audience:
  - "LEWG Library Evolution"
author:
  - name: Kirk Shoop
    email: <kirk.shoop@gmail.com>
  - name: Ville Voutilainen
    email: <ville.voutilainen@gmail.com>
toc: true
---

Introduction
============

This paper describes concepts that would be used to create and cleanup an object within an _`async-function`_ that will contain all _`async-function`_ s composed into that _`async-function`_. These _`async-function`_ s have access to a non-owning handle to the _`async-resource`_ that is safe to use. These _`async-function`_ s can be running on any execution context. An _`async-resource`_ object has only one concern, which is to open before any nested _`async-function`_ s start and to close after any nested _`async-function`_ complete. In order to be useful within other _`async-function`_ s, the object must not have any blocking functions.

An _`async-resource`_ can be thought of as an async-RAII object.

What is an _`async-resource`_?
----------------------------

An _`async-resource`_ is an object with state that is valid for all the 
_`async-function`_ s that are nested within the _`async-resource`_ expression. 

Examples include:

 - thread
 - thread-pool
 - io-pool
 - buffer-pool
 - mutex
 - file
 - socket
 - counting-scope [@P2519R0]

Motivation
==========

It is becoming apparent that all the sender/receiver features are language features being implemented in library.

Sender/Receiver itself is the implementation of an _`async-function`_. It supports values and errors and cancellation. It also requires manual memory management in implementations because _`async-resource`_ s do not fit inside any single block in the language.

A major precept of [@P2300R6] is structured concurrency. The `let_value()` algorithm provides stable storage for values produced by the input _`async-function`_. What is missing is a way to attach an object to a sender expression such that the object is opened before nested _`async-function`_ s start and is closed after nested _`async-function`_ s complete. This is commonly done in async programs using `std::shared_ptr` to implement ad-hoc garbage collection. Using garbage collection for this purpose removes structure from the code, because the shared ownership is unstructured and allows objects to escape the original scope in which they were created.

The C++ language has a set of rules that are applied in a code-block to describe when construction and destruction occur, and when values are valid. The language implements those rules.

This paper describes how to implement rules for the construction and destruction of async objects in the library. This paper describes structured construction and destruction of objects in terms of _`async-function`_ s. The `use_resources()` algorithm described in this paper is a library implementation of an async code-block containing one or more local variables. The `use_resources()` algorithm is somewhat analogous to the `using` keyword in some languages.

Design
======

What are the requirements for an _`async-resource`_?
--------------------------------------------------

**async construction**

Some objects have _`async-function`_ s to establish a connection, open a file, etc..

The design must allow for _`async-function`_ s to be used during construction - without blocking any threads (this means that C++ constructors are unable to meet this requirement)

**async destruction**

Some objects have _`async-function`_ s to teardown a connection, flush a file, etc..

The design must allow for _`async-function`_ s to be used during destruction - without blocking any threads (this means that C++ destructors are unable to meet this requirement)

**structured and correct-by-construction**

These are derived from the rules in the language.

The object will not be available until it has been constructed. 
The object will not be available until the object is contained in an _`async-function`_.
Failures of _`async-construction`_ and _`async-destruction`_ will complete to the containing _`async-function`_ with the error.
The object will always complete cleanup before completing to the containing _`async-function`_.
Acquiring an object is a no-fail _`async-function`_.

**composition**

Multiple object will be available at the same time without nesting.
Composition will support concurrent _`async-construction`_ of multiple objects.
Composition will support concurrent _`async-destruction`_ of multiple objects.
Dependencies between objects will be expressed by nesting.
Concurrency between objects will be expressed by algorithms like `when_all` and `use_resources()`.

What is the concept that an _`async-resource`_ must satisfy?
----------------------------------------------------------

There are two options for defining the _`async-resource`_ concept that are described here. Either one will satisfy the requirements.

### Option A - `run()`, `open()`, and `close()`

This option uses three new CPOs in two concepts that describe the lifetime 
of an _`async-resource`_.

The `open()` and `close()` _`async-function`_ s do no work and never complete with an error. The `open()` and `close()` _`async-function`_ s provide access to signals from the internal states of the `run()` _`async-function`_ before it completes.

This option depends only on [@P2300R6]

#### `async_resource` Concept:

An _`async-resource`_ stores the state used to open and run a resource.

```cpp
/// @brief the async-resource concept definition
template<class _T>
concept async_resource = 
  requires (const _T& __t_clv, _T& __t_lv){
    open_t{}(__t_clv);
    run_t{}(__t_lv);
  };

using open_t = /*implementation-defined/*;
/// @brief the open() cpo provides a sender that will complete with a async-resource-token. 
/// @details The async-resource-token will be valid until the sender provided 
/// by close() is started. 
/// The sender provided by open() will complete after the sender provided by 
/// run() has completed any async-function needed to open the resource.
/// The sender provided by open() will not fail.
/// @param async-resource&  
/// @returns sender<resource-token>
/**/
inline static constexpr open_t open{};

using run_t = /*implementation-defined/*;
/// @brief the run() cpo provides a sender-of-void. 
/// @details The sender provided by run() will start any async-function 
/// needed to open the resource and when all those async-functions complete 
/// then run() will complete the sender provided by open().
/// The sender provided by run() will complete after the sender provided 
/// by close() is started and all the async-functions needed to close 
/// the async-resource complete and the sender provided by close() is completed. 
/// @param async-resource&  
/// @returns sender<>
/**/
inline static constexpr run_t run{};
```

#### `async_resource_token` Concept:

An _`async-resource-token`_ is a non-owning handle to the resource that is provided after the resource has been opened.

The token must be used to close the resource once the resource has been opened.

```cpp
/// @brief the async-resource-token concept definition
template<class _T>
concept async_resource_token =
  requires (const _T& __t_clv){
    close_t{}(__t_clv);
  };

using close_t = /*implementation-defined/*;
/// @brief the close() cpo provides a sender-of-void. 
/// @details The sender provided by close() will trigger the sender provided 
/// by run() to begin any async-function needed to close the resource and 
/// will complete when all the async-function complete. 
/// The sender provided by close() will not fail.
/// @param async-resource-token&  
/// @returns sender<>
/**/
inline static constexpr close_t close{};
```

### Option B - `run() -> ` _`sequence-sender`_

This option uses one new CPO in one concept that describes the lifetime 
of an _`async-resource`_.

This option depends on a paper that adds _`sequence-sender`_ on top of [@P2300R6]

```cpp
/// @brief the async-resource concept definition
template<class _T>
concept async_resource = 
  requires (_T& __t){
    run_t{}(__t);
  };

using run_t = /*implementation-defined/*;
/// @brief the run() cpo provides a sequence-sender-of-token. 
/// @details The sequence-sender provided by run() will produce 
/// a run-function. When the run-function is started, it will start  
/// any async-function that are needed to open the async-resource. 
/// After all those async-function complete, the run-function 
/// will produce an async-resource-token as the only item in the 
/// sequence.
/// After the sender-expression for the async-resource-token item 
/// completes, the run-function will start any async-function 
/// that are needed to close the async-resource. 
/// After all those async-function complete, the run-function 
/// will complete. 
/// @param async-resource&  
/// @returns sequence-sender<async-resource-token>
/**/
inline static constexpr run_t run{};
```

What is the rational for this design?
-------------------------------------

This rational targets Option A. Exchanging the `open()` mentions for the emission of the _`async-resource-token`_ item and `close()` mentions for the completion of the sender expression that uses the _`async-resource-token`_ will describe the rational for Option B.

### run(), open(), and close()

The rationale for this design is that it unifies and generalizes asynchronous construction and destruction, making the construction adaptable via sender algorithms. Its success cases are handled by what follows an `open()` _`async-function`_, its failure cases are handled as results of the `run()` _`async-function`_. The success case isn't run at all if `run()` fails (which completes `open()` with `set_stopped()`), quite like what follows RAII initialization isn't performed if the RAII initialization fails.

Furthermore, asynchronous resources are acquired only when needed by asynchronous work, and that acquisition can itself be asynchronous. As a practical example, consider a thread pool that has a static amount of threads in it. With this approach, the threads can be spun up when needed by asynchronous work, and no sooner - and the threads are spun up asynchronously, without blocking, but the "success" case, i.e. the code that uses the threads, is run after the threads have been spun up.

The communication between `open()` and `run()` on an asynchronous resource is implicit, as is the communication between `close()` and `run()` on an asynchronous resource. The rationale for this choice is that it more closely models how language-level scopes work - you don't need to 'connect' the success code to a preceding RAII initialization, the success code just follows the RAII initialization once the initialization is complete. Likewise, there's no need to 'connect' `close()` to a completion of `run()`, that happens implicitly, quite like destruction implicitly follows exiting a scope. 

How do these CPOs compose to provide an async resource?
-------------------------------------------------------

### run(), open(), and close()

The `open()` _`async-function`_ and the `run()` _`async-function`_ are invoked concurrently.

After both of the `open()` and `run()` _`async-function`_ s are started, `run()` 
invokes any _`async-function`_ that is needed to initialize the _`async-resource`_. 

After all those _`async-function`_ s complete, then `run()` signals to `open()` which then will complete with the _`async-resource-token`_.

`run()` will complete after the following steps:

- the runtime has entered the `main()` function (requires a signal from the runtime)
- any _`async-function`_ needed to open the _`async-resource`_ has completed

  **at this point, the _`async-resource`_ lifetime begins**

- `open()` completes with the _`async-resource-token`_
- a stop condition is encountered
  - a `stop_token`, provided by the environment that invoked `open()`, is in the 
    `stop_requested()` state 
  
  **`OR`** 
  
  - the `close()` _`async-function`_ has been invoked 
  
  **`OR`** 
  
  - the runtime has exited the `main()` function (this requires a signal 
    from the runtime)

  **at this point, the _`async-resource`_ lifetime ends**

- any _`async-function`_ needed to close the _`async-resource`_ have completed

- `close()` completes

#### Activity diagram 

```plantuml
!pragma layout smetana
title run(), open(), and close() activity

(*) -->[invoke] "open(async-resource) [enter]"
-->[signal] "run(async-resource) [enter]"

(*) -->[invoke] "run(async-resource) [enter]"
-->[invoke] "asynchronous construction"

-->[complete] "open(async-resource) [ready]"
-->[complete] "let(open(async-resource), \n  [](async-resource-token){\n    return use-token-sender;})" 

-->[invoke] "spawn(async-resource-token, sender)"

-->[invoke] "use-token-sender" 

-->[invoke] "close(async-resource-token) [exit]"

-->[signal] "run(async-resource) [leave]"

-->[invoke] "asynchronous destruction" 
"spawn(async-resource-token, sender)" -->[join] "asynchronous destruction"

-->[complete] "close(async-resource-token) [closed]" 

-->[complete] "run(async-resource) [destruct]"

-->[complete] "destructed object"

-->(*)
```

### run() -> _`sequence-sender`_

`run()` is an _`async-function`_ aka _`run-function`_.

After the _`run-function`_ is invoked, it starts any _`async-function`_ 
that are needed to initialize the _`async-resource`_. 

After all those _`async-function`_ complete, the _`run-function`_ will 
emit the _`async-resource-token`_ as the only item in the sequence.

The _`run-function`_, will complete after the following steps:

- the runtime has entered the `main()` function (this requires a signal from the runtime)
- any _`async-function`_ needed to open the _`async-resource`_ has completed

  **at this point, the _`async-resource`_ lifetime begins**

- the _`async-resource-token`_ item is emitted
- a stop condition is encountered
  - a `stop_token`, provided by the environment of the _`open-function`_, is in the 
    `stop_requested()` state 

  **`OR`** 

  - the _`token-operation`_, produced by the sender expression for 
    the _`async-resource-token`_ item, has completed 

  **`OR`** 

  - the runtime has exited the `main()` function (this requires a signal from the runtime)

  **at this point, the _`async-resource`_ lifetime ends**

- any _`async-function`_ needed to close the _`async-resource`_ have completed

#### Activity diagram 

```plantuml
!pragma layout smetana
title "run() -> sequence-sender activity"

(*) -->[invoke] "run(async-resource) [enter]"
-->[invoke] "asynchronous construction"
-->[invoke] "use-token-sender = set_next(async-resource-token-sender)" 

-->[invoke] "use-token-sender"

-->[invoke] "spawn(async-resource-token, sender)"
-->[join] "asynchronous destruction"

"use-token-sender" -->[completed] "run(async-resource) [leave]"

-->[invoke] "asynchronous destruction"

-->[completed] "destructed object"

-->(*)
```

How do you use an _`async-resource`_?
-----------------------------------

Here is a basic example of composing resources using this pattern:

::: tonytable

> basic example

### run(), open(), and close()

```cpp
int main() {
  exec::static_thread_pool_resource ctx{1};
  exec::counting_scope_resource context;
  auto use = ex::when_all(
      exec::open(ctx), 
      exec::open(context)) | 
    ex::let_value([&](
      auto sch, auto scope){
        // async-resource lifetime begins

        sender auto begin = 
          ex::schedule(sch);

        sender auto printVoid = 
          ex::then(begin,
            []()noexcept { printf("void\n"); });

        exec::spawn(scope, printVoid);
        
        // async-resource lifetime ends
        // when close starts
        return ex::when_all(
          exec::close(sch), exec::close(scope));
      });
  ex::sync_wait(ex::when_all(
    use, 
    exec::run(ctx),
    exec::run(context)));
}
```

### run() -> _`sequence-sender`_

```cpp
int main() {
  exec::static_thread_pool_resource ctx{1};
  exec::counting_scope_resource context;
  auto use = ex::zip(
      exec::run(ctx), 
      exec::run(context)) | 
    ex::let_value_each([&](
      auto sch, auto scope){
        // async-resource lifetime begins

        sender auto begin = 
          ex::schedule(sch);

        sender auto printVoid = 
          ex::then(begin,
            []()noexcept { printf("void\n"); });

        exec::spawn(scope, printVoid);
        
        // async-resource lifetime ends 
        // when printVoid completes
        return printVoid;
      });
  ex::sync_wait(use);
}
```

:::

This pattern correctly scopes the use of the _`async-resource`_ and composes
the open, run, and close _`async-function`_ s correctly.

It is possible to compose multiple _`async-resource`_ s into the same block or
expression.

::: tonytable

> multiple _async-resource_ composition example

### run(), open(), and close()

```cpp
stop_source_resource stp;
static_thread_pool_resource ctx{1};
async_allocator_resource aa;
counting_scope_resource as;
async_socket_resource askt;
split_resource spl;

auto use = when_all(
  open(stp), open(ctx), open(aa), 
  open(as), open(askt), open(spl))
  | let_value([](
    stop_token stop, auto sched, auto alloc, 
    auto scope, auto sock, auto splt){
    auto env = make_env(empty_env{}, 
      with(get_stop_token, stop), 
      with(get_scheduler, sched), 
      with(get_async_allocator, alloc), 
      with(get_async_scope, scope));
    auto [input, output] = splt;
    auto producer = produce(input, 
      async_read_some(sock, MAX_DATA_SIZE));
    for (int i = 0; i < 4; ++i) {
      spawn(scope, 
        with_env(env, 
          on(sched, consume(output))));
    }
    auto close = when_all(
      close(stop), close(sched), close(alloc), 
      close(scope), close(sock), close(splt));
    return finally(
      with_env(env, 
        when_all(producer, 
          nest(consume(output)))), 
      close);
  });

std::this_thread::sync_wait(
  when_all(
    use,
    run(stp), run(ctx), run(aa), 
    run(as), run(askt), run(spl)));
```

### run() -> _`sequence-sender`_

```cpp
stop_source_resource stp;
static_thread_pool_resource ctx{1};
async_allocator_resource aa;
counting_scope_resource as;
async_socket_resource askt;
split_resource spl;

auto use = zip(
  run(stp), run(ctx), run(aa), 
  run(as), run(askt), run(spl))
  | let_value_each([](
    stop_token stop, auto sched, auto alloc, 
    auto scope, auto sock, auto splt){
    auto env = make_env(empty_env{}, 
      with(get_stop_token, stop), 
      with(get_scheduler, sched), 
      with(get_async_allocator, alloc), 
      with(get_async_scope, scope));
    auto [input, output] = splt;
    auto producer = produce(input, 
      async_read_some(sock, MAX_DATA_SIZE));
    for (int i = 0; i < 4; ++i) {
      spawn(scope, 
        with_env(env, 
          on(sched, consume(output))));
    }
    return with_env(env, 
      when_all(producer, 
        nest(consume(output))));
  });

std::this_thread::sync_wait(use);
```

:::

Both of these options fall into a pattern as well. This pattern can be placed in an algorithm. The `use_resources` algorithm changes the above example to look like:

```cpp
std::this_thread::sync_wait(
  use_resources([](
      auto stop, auto sched, 
      auto alloc, auto scope, 
      auto sock, auto split){
        auto [input, output] = split;
        auto env = make_env(empty_env{}, 
          with(get_stop_token, stop), 
          with(get_scheduler, sched), 
          with(get_async_allocator, alloc), 
          with(get_async_scope, scope));
        auto producer = produce(input, async_read_some(sock, MAX_DATA_SIZE));
        for (int i = 0; i < 4; ++i) {
          spawn(scope, with_env(env, on(sched, consume(output))));
        }
        return with_env(env, when_all(producer, nest(consume(output))));
      }, 
      make_deferred<stop_source_resource>(),
      make_deferred<static_thread_pool_resource>(1),
      make_deferred<async_allocator_resource>(),
      make_deferred<counting_scope_resource>(),
      make_deferred<async_socket_resource>(),
      make_deferred<split>()));
```

Why this design?
================

There have been many, many design options explored for the `counting_scope` proposed in [@P2519R0]. We had a few variations of a single object with methods, then two objects with methods. It was at this point that a pattern began to form across `stop_source`/`stop_token`, _`execution-context`_/`scheduler`, _`async-scope`_/_`async-scope-token`_.

The patterns proposed in this design model RAII for objects used by _`async-function`_ s. 

In C++, RAII works by attaching the constructor and destructor to a block of code in a function. This pattern uses the `run()` _`async-function`_ to represent the block in which the object is contained. The `run()` _`async-function`_ satisfies the structure requirement (only nested _`async-function`_ s use the object) and satisfies the correct-by-construction requirement (the object is not available until the `run()` _`async-function`_ is started and the object is closed when all nested _`async-function`_ s complete).

## `run()`, `open()`, and `close()`

The `run()`/`open()`/`close()` option (Option A) uses an _`async-function`_ to store the object (`run()`), another _`async-function`_ to access the object (`open()`), and a final _`async-function`_ to stop the object (`close()`). `open()` is not a constructor and `close()` is not a destructor. `open()` and `close()` are signals. `open()` signals that the object is ready to use and `close()` signals that the object is no longer needed. Any errors encountered in the object cause the `run()` _`async-function`_ to complete with the error, but only after completing any cleanup _`async-function`_ s needed.

### The open cpo

`open()` does not perform a task, its completion is a signal that `run()` has successfully constructed the resource.

Existing resources, like `run_loop` and `stop_source` have a method that returns a token. This does not provide for any _`async-function`_ s that are required before a token is valid.

`open()` is an _`async-function`_ that provides the token only after token is valid.

`open()` completes when the token is valid. All _`async-function`_ s using the token must be nested within the `run()` _`async-function`_ (yes, it is the run _`async-function`_ that owns the resource, not the open _`async-function`_).

The receiver passed to the `open()` _`async-function`_ is used to query services as needed (allocator, scheduler, _`stop-token`_, etc..)

### The run cpo

`open()` may start before the resource is constructed and completes when the token is valid. `run()` starts before the resources is constructed and completes after the token is closed. The `run()` _`async-function`_ represents the entire resource. The `run()` _`async-function`_ includes construction, open, resource usage, and close. `run()` is the owner of the resource, `open()` is the token accessor, `close()` is the signal to stop the resource.

`open()` cannot represent the resource because it will complete before the resource reaches the closed state.

`close()` cannot represent the resource because it cannot begin until after open has completed.

### The close cpo

`close()` does not perform a task, its invocation is a signal that requests that the resource safely destruct.

`close()` is used to start any _`async-function`_ s that stop the resource and invalidate the token. After the `close()` _`async-function`_ completes the `run()` _`async-function`_ runs the destructor of the resource and completes.

### Composition

The `open()` and `close()` cpos are not the only way to compose the token into a sender expression.

The benefit provided by the `open()` and `close()` _`async-function`_ s is that a `when_all` of multiple `open()`s and a `when_all` of multiple `close()`s can be used to access multiple tokens without nesting each token inside the prev.

### Structure

The `run()`, `open()`, and `close()` _`async-function`_ s provide the token in a structured manner. The token is not available until the `run()` _`async-function`_ has started and the `open()` _`async-function`_ has completed. The `run()` will not complete until the `close()` _`async-function`_ is started. This structure makes using the resource correct-by-construction. There is no resource until the `run()` and `open()` _`async-function`_ s are started. The `run()` _`async-function`_ will not complete until the `close()` _`async-function`_ completes.

Ordering of constructors and destructors is expressed by nesting resources explicitly. Using `when_all` to compose resources concurrently requires that the resources are independent because there is no token to the resource available until the `when_all` completes.

## `run() -> ` _`sequence-sender`_

The `run() -> ` _`sequence-sender`_ option (Option B), uses an _`async-function`_ to store the object (`run()`). The sequence produces one item that provides access to the object once it is ready to use. When the item has been consumed, `run()` will cleanup the object and complete. Any errors encountered in the object cause the `run()` _`async-function`_ to complete with the error, but only after completing any cleanup _`async-function`_ s needed.

### The run cpo

`run()` starts before the resource is constructed and completes after all nested _`async-function`_ s have completed and the object has finished any cleanup _`async-function`_. The `run()` _`async-function`_ represents the entire resource. The `run()` _`async-function`_ includes construction, resource usage, and destruction. `run()` is the owner of the resource.

### Composition

Composition is easily achieved using the `zip()` algorithm and the `let_value_each()` algorithm.

### Structure

The `run()` _`async-function`_ provides the object in a structured manner. The object is not available until the `run()` _`async-function`_ has started. The `run()` _`async-function`_ will not complete until the object is no longer in use. This structure makes using the resource correct-by-construction. There is no resource until the `run()` _`async-function`_ is invoked. The `run()` _`async-function`_ completes after all nested _`async-function`_ s have completed and the object has finished any cleanup _`async-function`_.

Ordering of constructors and destructors is expressed by nesting resources explicitly. Using the `zip()` algorithm to compose resources concurrently requires that the resources are independent because there is no token to the resource available until the `zip()` algorithm completes.

Implementation Experience
=========================

There are many implementations of _`async-scope`_. There is one in [@follygithub] and one in [@stdexecgithub]. Each is different, as the design space was being explored. Some are in use in large-scale production. 

Resources that implement both Option A and Option B have been implemented while writing this paper. Implementing resources with Option A is much more complicated and difficult than writing the same resource with Option B.

Prioritizing _`sequence-sender`_ will make Option B the obvious choice. 

Option A is a fallback in the case where _`sequence-sender`_ is delayed or rejected.

Entry and exit signals for `main()`
===================================

The steps for the `run()` _`async-function`_ on an _`async-resource`_ include requiring that `main()` has been entered before _`async-constructor`_ s are invoked and that `main()` exit must signal all _`async-resource`_ s when `main()` exits.

```cpp
// function object that returns a sender_of<void> that will 
// complete when main() is invoked and immediately when main() 
// has already been invoked
struct main_enter_t {
  /*@@_implementation-defined_@@*/ operator()() const noexcept;
};
static inline constexpr main_enter_t main_enter{};

// function object that returns a sender_of<void> that will 
// complete when main() has returned and immediately when main() 
// has already returned
struct main_exit_t {
  /*@@_implementation-defined_@@*/ operator()() const  noexcept;
};
static inline constexpr main_exit_t main_exit{};
```

Static Initialization Fiasco
----------------------------

The Static-Initialization-Fiasco is already harmful in single threaded programs, because there is no structure between translation units to order or scope static initialization. In multi threaded programs there are many additional issues caused by this lack of structure. For example:

- The destructor of a static object can be invoked on a different thread than the constructor was invoked. 

- The destructor of a static object can be invoked while other threads are still using the static object. 

- The destructor of a static object can be invoked and then accessed. 

These unstructured invocations on a static object then cause crashes, deadlocks, and even construction of new static object instances of already destructed objects during program exit.

The purpose of _`async-resource`_ is to create structure for asynchronous objects. Using _`async-function`_ s and _`async-resource`_ s to add structure to statically initialized objects can be used to impose structure across static objects for multi threaded and single threaded programs. 

System Execution Context
========================

[@P2079R2] proposes a `system_context` that provides access to system execution resources like Windows ThreadPool and MacOS GrandCentralDispatch. 

There are design discussions around how the `system_context` object itself will be accessed. It has been shown that at least one std library implementation can produce the system_context as a global object initialized prior to invoking static initializers. Allocators are similarly available before static initializers are invoked.

There may be _`async-scope`_ [@P2519R0] and _`stop-source`_ instances provided by the std library as well.

Now the pre-static-initialization objects (`allocator`, `stop_source`, `system_scope`, `system_context`) need to be structured. Which is allowed to depend on the others? In which order are the constructor and destructor of each invoked? How does a program that does not use one of these system objects prevent them from being constructed (pay only for what you use)?

The system objects are resources. Implementing system resource objects as _`async-resource`_ s provides structured construction and structured destruction and pay-only-for-what-you-use.


```cpp
class system_scheduler {
public:
  system_scheduler();
  ~system_scheduler();

  using self_t = system_scheduler; /*@@_exposition-only_@@*/

  system_scheduler(const self_t&);
  system_scheduler(self_t&&);
  self_t& operator=(const self_t&);
  self_t& operator=(self_t&&);

  bool operator==(const self_t&) const noexcept;

  // returns sender_of<void>
  /*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
    decays_to<self_t> auto&&, schedule_t) noexcept;
};

class system_execution {
public:
  system_execution();
  ~system_execution();

  using self_t = system_execution; /*@@_exposition-only_@@*/

  system_execution(const self_t&);
  system_execution(self_t&&);
  self_t& operator=(const self_t&);
  self_t& operator=(self_t&&);

  bool operator==(const self_t&) const noexcept;

  size_t /*@@_customization-point_@@*/(
    decays_to<self_t> const auto&, get_max_concurrency_t) noexcept;

  bool /*@@_customization-point_@@*/(
    decays_to<self_t> const auto&, get_completes_inline_t) noexcept;

  bool /*@@_customization-point_@@*/(
    decays_to<self_t> const auto&, 
    get_completes_before_invoke_returns_t) noexcept;

  bool /*@@_customization-point_@@*/(
    decays_to<self_t> const auto&, get_completes_on_same_t) noexcept;

  bool /*@@_customization-point_@@*/(
    decays_to<self_t> const auto&, get_completes_on_any_t) noexcept;

  system_scheduler /*@@_customization-point_@@*/(
    decays_to<self_t> const auto&, get_scheduler_t) noexcept;
};

class system_execution_resource {
public:
  system_execution_resource();
  ~system_execution_resource();

  using self_t = system_execution_resource; /*@@_exposition-only_@@*/

  system_execution_resource(const self_t&);
  system_execution_resource(self_t&&);
  self_t& operator=(const self_t&);
  self_t& operator=(self_t&&);

  using token_t = system_execution;

  // returns sequence_sender_of<token_t> (@@_async-resource_@@ Option B)
  /*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
    decays_to<self_t> auto&&, run_t) noexcept;
};
```

`std::thread` and `std::jthread`
================================

`std::thread` and `std::jthread` have a `join()` method that blocks the calling thread. This does not compose well. If you have two `std::thread`/`std::jthread` then you can only `join()` one at a time - **serially**. Even worse `std::jthread` hides `request_stop()` and `join()` in its destructor. Two  `std::jthread` in the same scope will not even invoke request scope on the second `std::jthread` that exits the scope, until the first has completely stopped and joined. If each thread was an _`async-resource`_, then there would be no `join()` method and no threads would be blocked and all threads exiting a scope would be stopped concurrently with all other _`async-resources`_ exiting the same scope.

```cpp
class thread {
public:
  thread();
  ~thread();

  using self_t = thread; /*@@_exposition-only_@@*/

  thread(const self_t&);
  thread(self_t&&);
  self_t& operator=(const self_t&);
  self_t& operator=(self_t&&);

  bool operator==(const self_t&) const noexcept;

  native_handle_type /*@@_customization-point_@@*/(
    decays_to<self_t> const auto&, get_native_handle_t) noexcept;

  id /*@@_customization-point_@@*/(
    decays_to<self_t> const auto&, get_id_t) noexcept;
};

class thread_resource {
public:
  thread_resource();
  ~thread_resource();

  using self_t = thread_resource; /*@@_exposition-only_@@*/

  thread_resource(const self_t&);
  thread_resource(self_t&&);
  self_t& operator=(const self_t&);
  self_t& operator=(self_t&&);

  using token_t = thread;

  // returns sequence_sender_of<token_t> (@@_async-resource_@@ Option B)
  /*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
    decays_to<self_t> auto&&, run_t) noexcept;
};
```

**advantages**

There is no way to `detach()` (the thread would become unstructured). 

There is no `join()` method (the thread implicitly joins when the body completes)

There is no embedded `std::stop_source` (an _`async-resource`_ has access to the _`stop-token`_ provided by the enclosing _`async-function`_).

`std::stop_source`
==================

`std::stop_source` allocates shared state (it is similar to `std::promise`). `std::stop_source` has complicated locking semantics around destruction of the source, token and callback objects.

```cpp
class inplace_stop_state {
public:
  inplace_stop_state();
  ~inplace_stop_state();

  using self_t = inplace_stop_state; /*@@_exposition-only_@@*/

  inplace_stop_state(const self_t&);
  inplace_stop_state(self_t&&);
  self_t& operator=(const self_t&);
  self_t& operator=(self_t&&);

  bool operator==(const self_t&) const noexcept;

  bool /*@@_customization-point_@@*/(
    decays_to<self_t> const auto&, get_stop_requested_t) noexcept;

  bool /*@@_customization-point_@@*/(
    decays_to<self_t> const auto&, get_stop_possible_t) noexcept;

  // returns sender_of<void>
  /*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
    decays_to<self_t> auto&&, stop_signal_t) noexcept;
};

class inplace_stop {
public:
  inplace_stop();
  ~inplace_stop();

  using self_t = inplace_stop; /*@@_exposition-only_@@*/

  inplace_stop(const self_t&);
  inplace_stop(self_t&&);
  self_t& operator=(const self_t&);
  self_t& operator=(self_t&&);

  bool operator==(const self_t&) const noexcept;

  inplace_stop_state /*@@_customization-point_@@*/(
    decays_to<self_t> const auto&, get_stop_state_t);

  bool /*@@_customization-point_@@*/(
    decays_to<self_t> const auto&, get_stop_requested_t) noexcept;

  bool /*@@_customization-point_@@*/(
    decays_to<self_t> const auto&, get_stop_possible_t) noexcept;

  // returns sender_of<void>
  /*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
    decays_to<self_t> auto&&, request_stop_t) noexcept;
};

class inplace_stop_resource {
public:
  inplace_stop_resource();
  ~inplace_stop_resource();

  using self_t = inplace_stop_resource; /*@@_exposition-only_@@*/

  inplace_stop_resource(const self_t&);
  inplace_stop_resource(self_t&&);
  self_t& operator=(const self_t&);
  self_t& operator=(self_t&&);

  using token_t = inplace_stop;

  // returns sequence_sender_of<token_t> (@@_async-resource_@@ Option B)
  /*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
    decays_to<self_t> auto&&, run_t) noexcept;
};
```

**advantages**

There is no allocation required.

The nested structure of the resource -> stop (source) -> stop-state (token) and the _`async-constructor`_ and _`async-destructor`_ combine to simplify the coordination between the objects.

There is no callback object required. The callback object is replaced by `stop_signal()` _`async-function`_.

`std::mutex`
============

`std::mutex` has a `lock()` method that blocks a thread and a `try_lock()` method that polls for a lock.

```cpp
class locked_mutex {
public:
  locked_mutex();
  ~locked_mutex();

  using self_t = locked_mutex; /*@@_exposition-only_@@*/

  locked_mutex(const self_t&);
  locked_mutex(self_t&&);
  self_t& operator=(const self_t&);
  self_t& operator=(self_t&&);

  bool operator==(const self_t&) const noexcept;

  bool /*@@_customization-point_@@*/(
    decays_to<self_t> const auto&, owns_lock_t) noexcept;

  // returns sender_of<mutex>
  /*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
    decays_to<self_t> auto&&, unlock_t) noexcept;
};

class locked_mutex_resource {
public:
  locked_mutex_resource();
  ~locked_mutex_resource();

  using self_t = locked_mutex_resource; /*@@_exposition-only_@@*/

  locked_mutex_resource(const self_t&);
  locked_mutex_resource(self_t&&);
  self_t& operator=(const self_t&);
  self_t& operator=(self_t&&);

  using token_t = locked_mutex;

  // returns sequence_sender_of<token_t> (@@_async-resource_@@ Option B)
  /*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
    decays_to<self_t> auto&&, run_t) noexcept;
};

class mutex {
public:
  mutex();
  ~mutex();

  using self_t = mutex; /*@@_exposition-only_@@*/

  mutex(const self_t&);
  mutex(self_t&&);
  self_t& operator=(const self_t&);
  self_t& operator=(self_t&&);

  bool operator==(const self_t&) const noexcept;

  locked_mutex_resource /*@@_customization-point_@@*/(
    decays_to<self_t> const auto&, lock_t);

  native_handle_type /*@@_customization-point_@@*/(
    decays_to<self_t> const auto&, get_native_handle_t) noexcept;
};

class mutex_resource {
public:
  mutex_resource();
  ~mutex_resource();

  using self_t = mutex_resource; /*@@_exposition-only_@@*/

  mutex_resource(const self_t&);
  mutex_resource(self_t&&);
  self_t& operator=(const self_t&);
  self_t& operator=(self_t&&);

  using token_t = mutex;

  // returns sequence_sender_of<token_t> (@@_async-resource_@@ Option B)
  /*@@_implementation-defined_@@*/ /*@@_customization-point_@@*/(
    decays_to<self_t> auto&&, run_t) noexcept;
};
```

**advantages**

There are no blocking functions.

The `locked_mutex_resource` provides an async-RAII region where the mutex is locked (like `std::unique_lock`)

`main()` 
========

When system resources are implemented as _`async-resource`_ a program that is safe for single threaded and multi threaded programs might implement `main()` in this way:

```cpp
int main() {
  using std::execution;

  auto application = [](
    system_scope scope, 
    this_thread::system_execution ex) {
      for (int i = 10; i > 0; --i) {
        spawn(scope, on(get_scheduler(ex), work{i}));
      }
      return just();
  };

  auto program = use_resources(application,
    make_deferred<system_scope_resource>();
    make_deferred<this_thread::system_execution_resource>());

  return this_thread::sync_wait(program);
}
```

Algorithms
==========

`make_deferred`
---------------

The `make_deferred` algorithm packages the constructor arguments for a potentially immovable type `T` and provides `void operator()()` that will construct `T` with the stored arguments when it is invoked.

The `make_deferred` algorithm returns a _`deferred-object`_ that contains storage for `T` and for `ArgN...`.

Before `T` is constructed, the _`deferred-object`_ copies and moves if the stored `ArgN...` supports those operations.

When the _`deferred-object`_ is invoked as a function taking no arguments, `T` is constructed in the reserved storage for `T` using the `ArgN...` stored in the _`deferred-object`_ when it was constructed.

Once `T` is constructed, attempts to copy and move the _`deferred-object`_ will `terminate()`.

Once `T` is constructed in the _`deferred-object`_, `T` can be accessed with `T& operator->()` and `T& value()` and eagerly destructed with `void reset()`.

```cpp
struct make_deferred_t {
  template<class T, class... ArgN>
  /*@@_implementation-defined_@@*/ operator()(ArgN&&... argN) const;
};
static inline constexpr make_deferred_t make_deferred{};
```

`use_resources`
---------------

The `use_resources` algorithm composes multiple _`async-resource`_ s into one _`async-function`_ that is returned as a sender.

The `use_resources` algorithm will use the selected option (Option A or Option B) to apply all the _`async-resource-token`_ s for the constructed _`async-resource`_ s to the single _`body-function`_.

When the returned _`async-function`_ is invoked, it will invoke all the deferred _`async-resource`_ s to construct them in its _`operation-state`_ and then it will acquire the _`async-resource-token`_ for each _`async-resource`_ and then invoke the _`body-function`_ once with all the tokens.

```cpp
struct use_resources_t {
  template<class Body, class... AsyncResourcesDeferred>
  /*@@_implementation-defined_@@*/ operator()(Body&& body, AsyncResourcesDeferred&&... resources) const;
};
static inline constexpr use_resources_t use_resources{};
```

Appendices
==========

Rejected Options:
-----------------

- `join() -> sender`:

  - `join()` returns a sender that completes after running any pending _`async-function`_ s followed by invoking any _`async-function`_ s needed to close the
_`async-resource`_.

  - This option is challenging because it is not correct by construction:

    - Imposes that all users remember to compose `join()` into an `counting_scope`
      and prevent the destructor from running until `join()` completes.
    - Provides no way to invoke _`async-function`_ s to open the _`async-resource`_.

- `run((token)->sender) -> sender`:

  - The sender returned from `run()` will complete after the following steps:

    - _`async-function`_ s to open the _`async-resource`_

      ** at this point, _`async-resource`_ lifetime begins **
    
    - an _`async-resource-token`_ is passed to the provided function
    - the sender returned from the provided function
    
      ** at this point, _`async-resource`_ lifetime ends **

    - any _`async-function`_ s needed to close the _`async-resource`_

  - This option scopes the use of the _`async-resource`_ and composes the open and 
    close _`async-function`_ s correctly.

  - It is hard to compose multiple _`async-resource`_ s into the same block or
    expression (requires nesting calls to `run()` for each _`async-resource`_, which
    also sequences the open and close for each _`async-resource`_).

The order of invocations when using an _`async-resource`_
---------------------------------------------------------

### One resource

#### run(), open(), and close()

```plantuml
title "async-resource open/run/close"
caption "sequence diagram for async-resource"
autonumber
participant "enclosing" as enc
participant "storage-op" as strg
participant "storage-fn" as sfn
participant "when-all-op" as all
participant "run-op" as run
participant "open-op" as open
participant "opened-fn" as ofn
participant "close-op" as close
participant "async-resource" as ar
activate enc
group construct the async-resource
enc -> strg ++ : start ""let_value(""\n""  just(""\n""    make_deferred<""\n""      counting_scope>()), ""\n""  storage-fn)""
strg -> sfn ++ : ""storage-fn(ctng-scp)""
sfn -> ar ++ : ""ctng-scp()""
ar --> sfn : enter constructed..
sfn --> strg -- : ""return when_all(""\n""  run(ctng-scp), ""\n""  open(ctng-scp) | ..)""
end
group compose run, open, and close using ""when_all""
strg -> all ++ : start ""when_all(""\n""  run(ctng-scp), ""\n""  open(ctng-scp) | ..)""
par
group run the async-resource
all -> run ++ : start ""run(ctng-scp)""
run -> ar : enter pending..
end
else
group open the async-resource
all -> open ++ : start ""let_value(open(ctng-scp), opened-fn)""
open -> ar : wait for any async functions needed to open
end
end
... wibbily wobbly timey wimey stuff ...
ar --> open : enter running..
group use the async-resource
open -> ofn ++ : ""opened-fn(ctng-tkn)""
ofn -> ar : ""spawn(ctng-tkn, on(sch, just()))""
ofn --> open -- : ""return close(ctng-tkn)""
end
group close the async-resource
open -> close ++ : start ""close(ctng-tkn)""
close -> ar : wait for all \nspawned functions \nto stop and any async \nfunctions needed to close
... wibbily wobbly timey wimey stuff ...
ar --> close : enter closed..
close --> open -- : complete close
open --> all -- : complete open
ar --> run : 
run --> all -- : complete run
end
all --> strg -- : complete when_all
end
ar --> strg -- : 
strg --> enc -- : complete storage
deactivate enc
```

#### `run() -> ` _`sequence-sender`_

```plantuml
title "async-resource run() -> sequence-sender"
caption "sequence diagram for async-resource"
autonumber
participant "enclosing" as enc
participant "storage-op" as strg
participant "storage-fn" as sfn
participant "let-value-each-op" as all
participant "tkn-fn" as tfn
participant "token-op" as tkn
participant "run-op" as run
participant "async-resource" as ar
activate enc
enc -> strg ++ : start ""let_value(""\n""  just(""\n""    make_deferred<""\n""      counting_scope>()), ""\n""  storage-fn)""
strg -> sfn ++ : ""storage-fn(ctng-scp)""
sfn --> strg -- : return ""let_value_each(""\n""  run(ctng-scp), tkn-fn))""
strg -> all ++ : start ""let_value_each(""\n""  run(ctng-scp), tkn-fn))""
group run the async-resource
all -> run ++ : start ""run(ctng-scp)""
group construct the async-resource
run -> ar ++ : ""ctng-scp()""
run -> ar : invoke any async functions needed to open
... wibbily wobbly timey wimey stuff ...
ar <-- ar : complete open functions
ar --> run : running..
run --> all : ""set_next ctng-tkn""
end
group use the async-resource
all -> tfn ++ : ""tkn-fn(ctng-tkn)""
tfn -> ar : ""spawn(ctng-tkn, ""\n""  on(sch, just(0)))""
tfn --> all -- : return ""on(sch, just(1))""
all -> tkn ++ : start ""on(sch, just(1))""
... wibbily wobbly timey wimey stuff ...
ar <-- ar : complete ""on(sch, just(0))""
tkn --> all : completed ""on(sch, just(1))""
tkn --> run -- : token function completed
ar --> run : spawned functions completed 
end
group close the async-resource
run -> ar : invoke any async functions needed to close
... wibbily wobbly timey wimey stuff ...
ar <-- ar : complete close functions
ar --> run : closed..
run -> ar : ""~ctng-scp()""
deactivate ar
run --> all -- : complete ""run(ctng-scp)""
end
end
all --> strg -- : complete ""let_value_each(""\n""  run(ctng-scp), tkn-fn))""
strg --> enc -- : complete ""let_value(""\n""  just(""\n""    make_deferred<""\n""      counting_scope>()), ""\n""  storage-fn)""
deactivate enc
```

### N resources

```plantuml
title "async-resource open/run/close"
caption "sequence diagram for async-resource"
autonumber
participant "enclosing" as enc
participant "storage-op" as strg
participant "storage-fn" as sfn
participant "when-all-op" as all
participant "run-op..." as run
participant "open-op..." as open
participant "opened-fn" as ofn
participant "close-op..." as close
participant "async-resource..." as ar
activate enc
group construct the async-resource
enc -> strg ++ : start ""let_value(""\n""  just(deferred-res...), ""\n""  storage-fn)""
strg -> sfn ++ : ""storage-fn(res...)""
sfn -> ar ++ : ""(res(), ...)""
ar --> sfn : enter constructed..
sfn --> strg -- : ""return when_all(""\n""  run(res)..., ""\n""  let_value(""\n""    when_all(""\n""      open(res)...), ""\n""    ..))""
end
group compose run, open, and close using ""when_all""
strg -> all ++ : start ""when_all(""\n""  run(res)..., ""\n""  let_value(""\n""    when_all(""\n""      open(res)...), ""\n""    ..))""
par
group run the async-resource
all -> run ++ : start ""run(res)...""
run -> ar : enter pending..
end
else
group open the async-resource
all -> open ++ : start ""let_value(""\n""    when_all(""\n""      open(res)...), ""\n""    opened-fn)""
open -> ar : wait for any async functions needed to open
end
end
... wibbily wobbly timey wimey stuff ...
ar --> open : enter running..
group use the async-resource
open -> ofn ++ : ""opened-fn(tkn...)""
ofn -> ar : spawn, schedule etc..
ofn --> open -- : ""return when_all(close(tkn)...)""
end
group close the async-resource
open -> close ++ : start ""when_all(close(tkn)...)""
close -> ar : wait for any async \nfunctions needed to close
... wibbily wobbly timey wimey stuff ...
ar --> close : enter closed..
close --> open -- : complete ""when_all(close)""
open --> all -- : complete ""when_all(open)""
ar --> run : 
run --> all -- : complete ""when_all(run)""
end
all --> strg -- : complete ""when_all""
end
ar --> strg -- : 
strg --> enc -- : complete storage
deactivate enc
```

### use_resource

```plantuml
title "use_resource composes async-resources"
caption "sequence diagram for use_resource"
autonumber
participant "enclosing" as enc
participant "use_resources-op" as use
participant "opened-fn" as ofn
participant "async-resource" as ar
activate enc
enc -> use ++ : start ""use_resources(""\n""  opened-fn, ""\n""  make_deferred<counting_scope>())""
use -> ar ++ : construct and open the resource
... wibbily wobbly timey wimey stuff ...
use -> ofn ++ : ""opened-fn(ctng-tkn)""
ofn --> ar : ""spawn(ctng-tkn, on(sch, just()))""
ofn --> use -- : ""return just()""
use -> ar : wait for all \nspawned functions \nto stop and any async \nfunctions needed to close
... wibbily wobbly timey wimey stuff ...
ar --> use -- : 
use --> enc -- : complete use_resources
deactivate enc
```

---
references:
  - id: stdexecgithub
    citation-label: stdexecgithub
    title: "stdexec"
    url: https://github.com/NVIDIA/stdexec
  - id: follygithub
    citation-label: follygithub
    title: "folly"
    url: https://github.com/facebook/folly
  - id: P2519R0
    citation-label: P2519R0
    type: paper
    title: "async_scope – Creating scopes for non-sequential concurrency"
    author:
      - family: Shoop
        given: Kirk
      - family: Howes
        given: Lee
      - family: Teodorescu
        given: Lucian Radu
    url: https://wg21.link/P2519R0
---
