#include <catch2/catch.hpp>
#include <async_scope.hpp>

namespace ex = std::execution;

TEST_CASE("async_scope can be created and them immediately destructed", "[dtor]") {
    ex::async_scope scope;
    (void)scope;
}
