#include <catch2/catch.hpp>
#include <async_scope.hpp>
#include "test_common/schedulers.hpp"

namespace ex = std::execution;
using std::this_thread::sync_wait;

TEST_CASE("calling request_stop will cancel the async_scope object", "[stop]") {
    ex::async_scope scope;

    scope.request_stop();

    REQUIRE(std::execution::__scope::empty(scope));
    impulse_scheduler sch;
    scope.spawn(ex::on(sch, ex::just()));
    REQUIRE(std::execution::__scope::empty(scope));
}
TEST_CASE("calling request_stop will be visible in stop_source", "[stop]") {
    ex::async_scope scope;

    scope.request_stop();
    REQUIRE(scope.get_stop_source().stop_requested());
}
TEST_CASE("calling request_stop will be visible in stop_token", "[stop]") {
    ex::async_scope scope;

    scope.request_stop();
    REQUIRE(scope.get_stop_token().stop_requested());
}

TEST_CASE("TODO: cancelling the associated stop_source will cancel the async_scope object",
        "[stop]") {
    ex::async_scope scope;

    scope.get_stop_source().request_stop();

    REQUIRE(std::execution::__scope::empty(scope));
    impulse_scheduler sch;
    scope.spawn(ex::on(sch, ex::just()));
    // TODO: the scope needs to be empty
    // REQUIRE(std::execution::__scope::empty(scope));
    REQUIRE_FALSE(std::execution::__scope::empty(scope));

    // TODO: remove this after ensuring that the work is not in the scope anymore
    sch.start_next();
}
TEST_CASE("cancelling the associated stop_source will be visible in stop_token", "[stop]") {
    ex::async_scope scope;

    scope.get_stop_source().request_stop();
    REQUIRE(scope.get_stop_token().stop_requested());
}
