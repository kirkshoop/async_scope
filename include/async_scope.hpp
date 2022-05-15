#pragma once

#include <execution.hpp>

namespace std::execution {

namespace __event {

struct _op_base;

template <typename Receiver>
struct _operation {
    struct type;
};

template <typename Receiver>
using operation = typename _operation<Receiver>::type;

struct async_manual_reset_event;

// Sender type that is triggered when the async_object becomes empty
// The async_scope::on_empty() method returns a wrapped version of this
struct __sender {

    explicit __sender(const async_manual_reset_event& evt) noexcept
        : evt_(&evt) {}

    template <__decays_to<__sender> _Self, receiver _Receiver>
    requires environment_provider<_Receiver> &&
            receiver_of<_Receiver, completion_signatures_of_t<_Self, env_of_t<_Receiver>>>
    friend auto tag_invoke(connect_t, _Self&& __self, _Receiver&& __rcvr)
            -> operation<remove_cvref_t<_Receiver>> {
        return operation<remove_cvref_t<_Receiver>>{*__self.evt_, (_Receiver &&) __rcvr};
    }

    template <__decays_to<__sender> _Self, class _Env>
    friend auto tag_invoke(get_completion_signatures_t, _Self&&, _Env)
            -> std::execution::completion_signatures<std::execution::set_value_t()>;

private:
    const async_manual_reset_event* evt_;
};

// Event object that is triggered when the async_scope is empty; set() will be called when
// async_scope becomes empty. If there are `on_empty()` senders registered, this will trigger
// them.
struct async_manual_reset_event {
    async_manual_reset_event() noexcept
        : async_manual_reset_event(false) {}

    explicit async_manual_reset_event(bool startSignalled) noexcept
        : state_(startSignalled ? this : nullptr) {}

    // Called when the async_scope reaches "empty" state
    void set() noexcept;

    // Called when the async_scope object is not empty anymore
    void reset() noexcept {
        // transition from signalled (i.e. state_ == this) to not-signalled
        // (i.e. state_ == nullptr).
        void* oldState = this;

        // We can ignore the the result.  We're using _strong so it won't fail
        // spuriously; if it fails, it means it wasn't previously in the signalled
        // state so resetting is a no-op.
        (void)state_.compare_exchange_strong(oldState, nullptr, std::memory_order_acq_rel);
    }

    // Returns a sender that is triggered when the async_scope becomes empty
    [[nodiscard]] __sender async_wait() const noexcept { return __sender{*this}; }

private:
    std::atomic<void*> state_{};

    friend struct _op_base;

    // note: this is a static method that takes evt *second* because the caller
    //       a member function on _op_base and so will already have op in first
    //       argument position; making this function a member would require some
    //       register-juggling code, which would increase binary size
    static void start_or_wait(_op_base& op, const async_manual_reset_event& evt) noexcept;
};

// Base class for an operation connecting a sender indicating the emptiness of async_scope and
// the receiver passed to async_scope::on_empty().
// This implements a type-erased operation. Derived classes will know about actual receiver
// types.
// We keep here in an intrinsic linked list the senders waiting for the emptiness of the
// asyc_scope object.
struct _op_base {
    // note: next_ is intentionally left indeterminate until the operation is
    //       pushed on the event's stack of waiting operations
    //
    // note: next_ and setValue_ are the first two members because this ordering
    //       leads to smaller code than others; on ARM, the first two members can
    //       be loaded into a pair of registers in one instruction, which turns
    //       out to be important in both async_manual_reset_event::set() and
    //       start_or_wait().
    _op_base* next_;
    void (*setValue_)(_op_base*) noexcept;
    const async_manual_reset_event* evt_;

    explicit _op_base(
            const async_manual_reset_event& evt, void (*setValue)(_op_base*) noexcept) noexcept
        : setValue_(setValue)
        , evt_(&evt) {}

    ~_op_base() = default;

    _op_base(_op_base&&) = delete;
    _op_base& operator=(_op_base&&) = delete;

    void set_value() noexcept { setValue_(this); }

    void start() noexcept { async_manual_reset_event::start_or_wait(*this, *evt_); }
};

// Final operation connecting __sender and the receiver to be notified about async_scope emptiness
template <typename Receiver>
struct _operation<Receiver>::type : private _op_base {
    explicit type(const async_manual_reset_event& evt, Receiver r) noexcept
        : _op_base(evt, &set_value_impl)
        , recv_(std::move(r)) {}

    ~type() = default;

    type(type&&) = delete;
    type& operator=(type&&) = delete;

private:
    friend void tag_invoke(start_t, type& self) noexcept { self.start(); }

    [[no_unique_address]] Receiver recv_;

    // Called when the async_object is empty
    static void set_value_impl(_op_base* base) noexcept {
        auto self = static_cast<type*>(base);
        std::execution::set_value(std::move(self->recv_));
    }
};

inline void async_manual_reset_event::set() noexcept {
    void* const signalledState = this;

    // replace the stack of waiting operations with a sentinel indicating we've
    // been signalled
    void* top = state_.exchange(signalledState, std::memory_order_acq_rel);

    if (top == signalledState) {
        // we were already signalled so there are no waiting operations
        return;
    }

    // We are the first thread to set the state to signalled; iteratively pop
    // the stack and complete each operation.
    auto op = static_cast<_op_base*>(top);
    while (op != nullptr) {
        std::exchange(op, op->next_)->set_value();
    }
}

inline void async_manual_reset_event::start_or_wait(
        _op_base& op, const async_manual_reset_event& evt) noexcept {
    async_manual_reset_event& e = const_cast<async_manual_reset_event&>(evt);
    // Try to push op onto the stack of waiting ops.
    void* const signalledState = &e;

    void* top = e.state_.load(std::memory_order_acquire);

    do {
        if (top == signalledState) {
            // Already in the signalled state; don't push it.
            op.set_value();
            return;
        }

        // note: on the first iteration, this line transitions op.next_ from
        //       indeterminate to a well-defined value
        op.next_ = static_cast<_op_base*>(top);
    } while (!e.state_.compare_exchange_weak(
            top, static_cast<void*>(&op), std::memory_order_release, std::memory_order_acquire));
}

} // namespace __event

namespace __scope {

struct async_scope;

struct _receiver_base {
    void* op_;
    async_scope* scope_;
};

template <typename _Sender>
struct __receiver {
    struct type;
};

template <typename _Sender>
using receiver = typename __receiver<_Sender>::type;

template <typename _Sender>
using _operation_t = connect_result_t<_Sender, receiver<_Sender>>;

template <typename _Sender>
struct __future_receiver {
    struct type;
};

template <typename _Sender>
using future_receiver = typename __future_receiver<_Sender>::type;

template <typename _Sender>
using _future_operation_t = connect_result_t<_Sender, future_receiver<_Sender>>;

void record_done(async_scope*) noexcept;
// For testing purposes
[[nodiscard]] bool empty(const async_scope& scope) noexcept;
[[nodiscard]] std::size_t op_count(const async_scope& scope) noexcept;

template <typename Sender>
struct __receiver<Sender>::type final : private receiver_adaptor<receiver<Sender>>, _receiver_base {

    template <typename Op>
    explicit type(Op* op, async_scope* scope) noexcept
        : receiver_adaptor<receiver<Sender>>{}
        , _receiver_base{op, scope} {
        static_assert(same_as<Op, std::optional<_operation_t<Sender>>>);
    }

    // receivers uniquely own themselves; we don't need any special move-
    // construction behaviour, but we do need to ensure no copies are made
    type(type&&) noexcept = default;

    ~type() = default;

    // it's just simpler to skip this
    type& operator=(type&&) = delete;

private:
    friend receiver_adaptor<receiver<Sender>>;

    void set_value() noexcept { set_stopped(); }

    [[noreturn]] void set_error(std::exception_ptr) noexcept { std::terminate(); }

    void set_stopped() noexcept {
        // we're about to delete this, so save the scope for later
        auto scope = scope_;
        auto op = static_cast<std::optional<_operation_t<Sender>>*>(op_);
        delete op;
        record_done(scope);
    }

    make_env_t<get_stop_token_t, never_stop_token> get_env() const& {
        return make_env<get_stop_token_t>(never_stop_token{});
    }
};

template <sender _Sender>
class __future_state;

namespace __impl {
struct __subscription {
    virtual void __complete_() noexcept = 0;
    __subscription* __next_ = nullptr;
};

template <typename _SenderId, typename _ReceiverId>
class __operation final : __subscription {
    using _Sender = __t<_SenderId>;
    using _Receiver = __t<_ReceiverId>;

    friend void tag_invoke(start_t, __operation& __op_state) noexcept { __op_state.__start_(); }

    void __complete_() noexcept override try {
        static_assert(sender_to<_Sender, _Receiver>);
        if (__state_->__data_.index() == 2 || get_stop_token(get_env(__rcvr_)).stop_requested()) {
            set_stopped((_Receiver &&) __rcvr_);
        } else if (__state_->__data_.index() == 1) {
            std::apply(
                    [this](auto&&... as) {
                        set_value((_Receiver &&) __rcvr_, ((decltype(as)&&)as)...);
                    },
                    std::move(std::get<1>(__state_->__data_)));
        } else {
            std::terminate();
        }
    } catch (...) {
        set_error((_Receiver &&) __rcvr_, current_exception());
    }

    void __start_() noexcept;

    [[no_unique_address]] _Receiver __rcvr_;
    std::unique_ptr<__future_state<_Sender>> __state_;

public:
    template <typename _Receiver2>
    explicit __operation(_Receiver2&& __rcvr, std::unique_ptr<__future_state<_Sender>> __state)
        : __rcvr_((_Receiver2 &&) __rcvr)
        , __state_(std::move(__state)) {}
};

template <sender _Sender>
using __future_result_t =
        execution::value_types_of_t<_Sender, __empty_env, execution::__decayed_tuple, __single_t>;
} // namespace __impl

template <typename _Sender>
struct __future_receiver<_Sender>::type final : private receiver_adaptor<future_receiver<_Sender>>,
                                                _receiver_base {

    template <typename State>
    explicit type(State* state, async_scope* scope) noexcept
        : receiver_adaptor<future_receiver<_Sender>>{}
        , _receiver_base{state, scope} {
        static_assert(same_as<State, __future_state<_Sender>>);
    }

    // receivers uniquely own themselves; we don't need any special move-
    // construction behaviour, but we do need to ensure no copies are made
    type(type&&) noexcept = default;

    ~type() = default;

    // it's just simpler to skip this
    type& operator=(type&&) = delete;

private:
    friend receiver_adaptor<future_receiver<_Sender>>;

    template <class _Sender2 = _Sender, class... _As>
    requires constructible_from<__impl::__future_result_t<_Sender2>, _As...>
    void set_value(_As&&... __as) noexcept try {
        auto& state = *reinterpret_cast<__future_state<_Sender>*>(op_);
        state.__data_.template emplace<1>((_As &&) __as...);
        dispatch_result();
    } catch (...) {
        std::terminate();
    }
    [[noreturn]] void set_error(std::exception_ptr) noexcept { std::terminate(); }
    void set_stopped() noexcept {
        auto& state = *reinterpret_cast<__future_state<_Sender>*>(op_);
        state.__data_.template emplace<2>(execution::set_stopped);
        dispatch_result();
    }

    void dispatch_result() {
        auto& state = *reinterpret_cast<__future_state<_Sender>*>(op_);
        while (auto* __sub = state.__pop_front_()) {
            __sub->__complete_();
        }
        record_done(scope_);
    }

    make_env_t<get_stop_token_t, never_stop_token> get_env() const& {
        return make_env<get_stop_token_t>(never_stop_token{});
    }
};

template <sender _Sender>
class __future;

template <sender _Sender>
class __future_state : std::enable_shared_from_this<__future_state<_Sender>> {
    template <class, class>
    friend class __impl::__operation;
    template <class>
    friend struct __future_receiver;
    template <sender>
    friend class __future;
    friend struct async_scope;

    using op_t = _future_operation_t<_Sender>;

    std::optional<op_t> __op_;
    variant<monostate, __impl::__future_result_t<_Sender>, execution::set_stopped_t> __data_;

    void __push_back_(__impl::__subscription* __task);
    __impl::__subscription* __pop_front_();

    mutex __mutex_;
    condition_variable __cv_;
    __impl::__subscription* __head_ = nullptr;
    __impl::__subscription* __tail_ = nullptr;
};

namespace __impl {
template <typename _SenderId, typename _ReceiverId>
inline void __operation<_SenderId, _ReceiverId>::__start_() noexcept try {
    if (!!__state_) {
        if (__state_->__data_.index() > 0) {
            __complete_();
        } else {
            __state_->__push_back_(this);
        }
    } else {
        set_stopped((_Receiver &&) __rcvr_);
    }
} catch (...) {
    set_error((_Receiver &&) __rcvr_, current_exception());
}
} // namespace __impl

template <sender _Sender>
inline void __future_state<_Sender>::__push_back_(__impl::__subscription* __subscription) {
    unique_lock __lock{__mutex_};
    if (__head_ == nullptr) {
        __head_ = __subscription;
    } else {
        __tail_->__next_ = __subscription;
    }
    __tail_ = __subscription;
    __subscription->__next_ = nullptr;
    __cv_.notify_one();
}

template <sender _Sender>
inline __impl::__subscription* __future_state<_Sender>::__pop_front_() {
    unique_lock __lock{__mutex_};
    if (__head_ == nullptr) {
        return nullptr;
    }
    auto* __subscription = __head_;
    __head_ = __subscription->__next_;
    if (__head_ == nullptr)
        __tail_ = nullptr;
    return __subscription;
}

template <sender _Sender>
class __future {
    friend struct async_scope;

    explicit __future(std::unique_ptr<__future_state<_Sender>> state) noexcept
        : state_(std::move(state)) {}

    template <typename _Receiver>
    friend __impl::__operation<__x<_Sender>, __x<decay_t<_Receiver>>> tag_invoke(
            connect_t, __future&& __self, _Receiver&& __rcvr) {
        return __impl::__operation<__x<_Sender>, __x<decay_t<_Receiver>>>{
                (_Receiver &&) __rcvr, std::move(__self.state_)};
    }

    template <__decays_to<__future> _Self, class _Env>
    friend auto tag_invoke(get_completion_signatures_t, _Self&&, _Env)
            -> completion_signatures_of_t<__member_t<_Self, _Sender>, _Env>;

private:
    std::unique_ptr<__future_state<_Sender>> state_;
};

struct async_scope {
private:
    template <typename Scheduler, typename Sender>
    using _on_result_t = decltype(on(__declval<Scheduler&&>(), __declval<Sender&&>()));

    in_place_stop_source stopSource_;
    // (opState_ & 1) is 1 until we've been stopped
    // (opState_ >> 1) is the number of outstanding operations
    std::atomic<std::size_t> opState_{1};
    __event::async_manual_reset_event evt_;

    [[nodiscard]] auto await_and_sync() const noexcept {
        return then(evt_.async_wait(), [this]() noexcept {
            // make sure to synchronize with all the fetch_subs being done while
            // operations complete
            (void)opState_.load(std::memory_order_acquire);
        });
    }

public:
    async_scope() noexcept = default;

    ~async_scope() {
        end_of_scope(); // enter stop state

        [[maybe_unused]] auto state = opState_.load(std::memory_order_relaxed);

        if (!is_stopping(state) || op_count(state) != 0) {
            std::terminate();
        }
    }

    template <typename _Sender>
    // requires sender_to<_Sender, receiver<std::remove_cvref_t<_Sender>>>
    void spawn(_Sender&& sender) {
        // this could throw; if it does, there's nothing to clean up
        auto opToStart =
                std::make_unique<std::optional<_operation_t<std::remove_cvref_t<_Sender>>>>();

        // this could throw; if it does, the only clean-up we need is to
        // deallocate the std::optional, which is handled by opToStart's
        // destructor so we're good
        opToStart->emplace(__conv{[&] {
            return connect((_Sender &&) sender,
                    receiver<std::remove_cvref_t<_Sender>>{opToStart.get(), this});
        }});

        // At this point, the rest of the function is noexcept, but opToStart's
        // destructor is no longer enough to properly clean up because it won't
        // invoke destruct().  We need to ensure that we either call destruct()
        // ourselves or complete the operation so *it* can call destruct().

        if (try_record_start()) {
            // start is noexcept so we can assume that the operation will complete
            // after this, which means we can rely on its self-ownership to ensure
            // that it is eventually deleted
            std::execution::start(**opToStart.release());
        }
    }

    template <typename _Sender>
    requires sender_to<_Sender, future_receiver<std::remove_cvref_t<_Sender>>>
            __future<std::remove_cvref_t<_Sender>> spawn_future(_Sender&& sender) {
        // this could throw; if it does, there's nothing to clean up
        auto state = std::make_unique<__future_state<std::remove_cvref_t<_Sender>>>();

        // this could throw; if it does, the only clean-up we need is to
        // deallocate the std::optional, which is handled by opToStart's
        // destructor so we're good
        state->__op_.emplace(__conv{[&] {
            return connect((_Sender &&) sender,
                    future_receiver<std::remove_cvref_t<_Sender>>{state.get(), this});
        }});

        // At this point, the rest of the function is noexcept, but opToStart's
        // destructor is no longer enough to properly clean up because it won't
        // invoke destruct().  We need to ensure that we either call destruct()
        // ourselves or complete the operation so *it* can call destruct().

        if (try_record_start()) {
            // start is noexcept so we can assume that the operation will complete
            // after this, which means we can rely on its self-ownership to ensure
            // that it is eventually deleted
            std::execution::start(*state->__op_);
            return __future<std::remove_cvref_t<_Sender>>{std::move(state)};
        }
        // __future will complete with set_stopped
        return __future<std::remove_cvref_t<_Sender>>{nullptr};
    }

    [[nodiscard]] auto on_empty() const noexcept { return await_and_sync(); }

    in_place_stop_source& get_stop_source() noexcept { return stopSource_; }

    in_place_stop_token get_stop_token() noexcept { return stopSource_.get_token(); }

    bool request_stop() noexcept {
        end_of_scope();
        return stopSource_.request_stop();
    }

private:
    static constexpr std::size_t stoppedBit{1};

    static bool is_stopping(std::size_t state) noexcept { return (state & stoppedBit) == 0; }

    static std::size_t op_count(std::size_t state) noexcept { return state >> 1; }

    // For testing purposes
    [[nodiscard]] friend bool empty(const async_scope& scope) noexcept {
        return async_scope::op_count(scope.opState_) == 0;
    }
    [[nodiscard]] friend std::size_t op_count(const async_scope& scope) noexcept {
        return async_scope::op_count(scope.opState_);
    }

    [[nodiscard]] bool try_record_start() noexcept {
        auto opState = opState_.load(std::memory_order_relaxed);

        do {
            if (is_stopping(opState)) {
                return false;
            }

            assert(opState + 2 > opState);
        } while (!opState_.compare_exchange_weak(opState, opState + 2, std::memory_order_relaxed));

        evt_.reset();

        return true;
    }

    friend void record_done(async_scope* scope) noexcept {
        auto oldState = scope->opState_.fetch_sub(2, std::memory_order_release);

        if (op_count(oldState) == 1) {
            // the last op to finish
            scope->evt_.set();
        }
    }

    void end_of_scope() noexcept {
        // stop adding work
        auto oldState = opState_.fetch_and(~stoppedBit, std::memory_order_release);

        if (op_count(oldState) == 0) {
            // there are no outstanding operations to wait for
            evt_.set();
        }
    }
};

} // namespace __scope

using __scope::async_scope;

} // namespace std::execution