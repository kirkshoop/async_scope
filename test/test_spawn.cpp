#include <catch2/catch.hpp>
#include <async_scope.hpp>
#include "test_common/schedulers.hpp"
#include "test_common/receivers.hpp"

namespace ex = std::execution;
using std::this_thread::sync_wait;

//! Sender that throws exception when connected
struct throwing_sender {
    using completion_signatures = ex::completion_signatures<ex::set_value_t()>;

    template <class Receiver>
    struct operation {
        Receiver rcvr_;

        friend void tag_invoke(ex::start_t, operation& self) noexcept {
            ex::set_value(std::move(self.rcvr_));
        }
    };

    template <class Receiver>
    friend auto tag_invoke(ex::connect_t, throwing_sender&& self, Receiver&& rcvr)
            -> operation<std::decay_t<Receiver>> {
        throw std::logic_error("cannot connect");
        return {std::forward<Receiver>(rcvr)};
    }
};

TEST_CASE("spawn will execute its work", "[spawn]") {
    impulse_scheduler sch;
    bool executed{false};
    ex::async_scope scope;

    // Non-blocking call
    scope.spawn(ex::on(sch, ex::just() | ex::then([&] { executed = true; })));
    REQUIRE_FALSE(executed);
    // Run the operation on the scheduler
    sch.start_next();
    // Now the spawn work should be completed
    REQUIRE(executed);
}

TEST_CASE("spawn will start sender before returning", "[spawn]") {
    bool executed{false};
    ex::async_scope scope;

    // This will be a blocking call
    scope.spawn(ex::just() | ex::then([&] { executed = true; }));
    REQUIRE(executed);
}

#if !NO_TESTS_WITH_EXCEPTIONS
TEST_CASE("spawn will propagate exceptions encountered during op creation", "[spawn]") {
    ex::async_scope scope;
    try {
        scope.spawn(throwing_sender{} | ex::then([&] { FAIL("work should not be executed"); }));
        FAIL("Exceptions should have been thrown");
    } catch (const std::logic_error& e) {
        SUCCEED("correct exception caught");
    } catch (...) {
        FAIL("invalid exception caught");
    }
}
#endif

TEST_CASE("spawn will keep the scope non-empty until the work is executed", "[spawn]") {
    impulse_scheduler sch;
    bool executed{false};
    ex::async_scope scope;

    // Before adding any operations, the scope is empty
    REQUIRE(std::execution::__scope::empty(scope));

    // Non-blocking call
    scope.spawn(ex::on(sch, ex::just() | ex::then([&] { executed = true; })));
    REQUIRE_FALSE(executed);

    // The scope is now non-empty
    REQUIRE_FALSE(std::execution::__scope::empty(scope));
    REQUIRE(std::execution::__scope::op_count(scope) == 1);

    // Run the operation on the scheduler; blocking call
    sch.start_next();

    // Now the scope should again be empty
    REQUIRE(std::execution::__scope::empty(scope));
    REQUIRE(executed);
}

TEST_CASE("spawn will keep track on how many operations are in flight", "[spawn]") {
    impulse_scheduler sch;
    std::size_t num_executed{0};
    ex::async_scope scope;

    // Before adding any operations, the scope is empty
    REQUIRE(std::execution::__scope::op_count(scope) == 0);
    REQUIRE(std::execution::__scope::empty(scope));

    constexpr std::size_t num_oper = 10;
    for (std::size_t i = 0; i < num_oper; i++) {
        scope.spawn(ex::on(sch, ex::just() | ex::then([&] { num_executed++; })));
        size_t num_expected_ops = i + 1;
        REQUIRE(std::execution::__scope::op_count(scope) == num_expected_ops);
    }

    // Now execute the operations
    for (std::size_t i = 0; i < num_oper; i++) {
        sch.start_next();
        size_t num_expected_ops = num_oper - i - 1;
        REQUIRE(std::execution::__scope::op_count(scope) == num_expected_ops);
    }

    // The scope is empty after all the operations are executed
    REQUIRE(std::execution::__scope::empty(scope));
    REQUIRE(num_executed == num_oper);
}

TEST_CASE("TODO: spawn work can be cancelled by cancelling the scope", "[spawn]") {
    impulse_scheduler sch;
    ex::async_scope scope;

    bool cancelled1{false};
    bool cancelled2{false};

    scope.spawn(ex::on(sch, ex::just() | ex::let_stopped([&] {
        cancelled1 = true;
        return ex::just();
    })));
    scope.spawn(ex::on(sch, ex::just() | ex::let_stopped([&] {
        cancelled2 = true;
        return ex::just();
    })));

    REQUIRE(std::execution::__scope::op_count(scope) == 2);

    // Execute the first operation, before cancelling
    sch.start_next();
    REQUIRE_FALSE(cancelled1);
    REQUIRE_FALSE(cancelled2);

    // Cancel the async_scope object
    scope.request_stop();
    REQUIRE(std::execution::__scope::op_count(scope) == 1);

    // Execute the first operation, after cancelling
    sch.start_next();
    REQUIRE_FALSE(cancelled1);
    // TODO: second operation should be cancelled
    // REQUIRE(cancelled2);
    REQUIRE_FALSE(cancelled2);

    REQUIRE(std::execution::__scope::empty(scope));
}

template <typename S>
concept is_spawn_worthy = requires(ex::async_scope& scope, S&& snd) {
    scope.spawn(std::move(snd));
};

TEST_CASE("spawn accepts void senders", "[spawn]") {
    static_assert(is_spawn_worthy<decltype(ex::just())>);
}
TEST_CASE("TODO: spawn doesn't accept non-void senders", "[spawn]") {
    // TODO: these should not be allowed
    static_assert(is_spawn_worthy<decltype(ex::just(13))>);
    static_assert(is_spawn_worthy<decltype(ex::just(3.14))>);
    static_assert(is_spawn_worthy<decltype(ex::just("hello"))>);
}
TEST_CASE("TODO: spawn doesn't accept senders of errors", "[spawn]") {
    // TODO: these should not be allowed
    static_assert(is_spawn_worthy<decltype(ex::just_error(std::exception_ptr{}))>);
    static_assert(is_spawn_worthy<decltype(ex::just_error(std::error_code{}))>);
    static_assert(is_spawn_worthy<decltype(ex::just_error(-1))>);
}
TEST_CASE("spawn should accept senders that send stopped signal", "[spawn]") {
    static_assert(is_spawn_worthy<decltype(ex::just_stopped())>);
}
TEST_CASE("spawn works with senders that complete with stopped signal", "[spawn]") {
    impulse_scheduler sch;
    ex::async_scope scope;

    REQUIRE(std::execution::__scope::empty(scope));

    scope.spawn(ex::on(sch, ex::just_stopped()));

    // The scope is now non-empty
    REQUIRE_FALSE(std::execution::__scope::empty(scope));
    REQUIRE(std::execution::__scope::op_count(scope) == 1);

    // Run the operation on the scheduler; blocking call
    sch.start_next();

    // Now the scope should again be empty
    REQUIRE(std::execution::__scope::empty(scope));
}
