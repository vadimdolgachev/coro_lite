//
// Created by vadim on 18.04.2026.
//

#include "CancellationToken.h"

#include "Logger.h"

namespace CoroLite {
    // A moved-from token has no state; treat it as stopped (fail-safe) instead of
    // dereferencing a null shared_ptr.
    bool CancellationToken::isStopped() const noexcept {
        return state == nullptr || state->stopped.load();
    }

    int CancellationToken::getFd() const noexcept {
        return state == nullptr ? -1 : state->signal.getFd();
    }

    void CancellationToken::drain() const noexcept {
        // Cancellation is irreversible: retain level readiness for every waiter.
        if (state != nullptr && !state->stopped.load()) {
            state->signal.drain();
        }
    }

    CancellationToken CancellationToken::clone() const noexcept {
        return CancellationToken(state);
    }

    CancellationToken::CancellationToken(std::shared_ptr<State> s) :
        state(std::move(s)) {
    }

    CancellationTokenSource::CancellationTokenSource() :
        state(std::make_shared<CancellationToken::State>()) {
    }

    CancellationToken CancellationTokenSource::getToken() const noexcept {
        return CancellationToken(state);
    }

    void CancellationTokenSource::requestStop() const noexcept {
        if (!state->stopped.exchange(true)) {
            state->signal.signal();
        }
    }

    bool CancellationTokenSource::isStopped() const noexcept {
        return state->stopped.load();
    }
} // namespace CoroLite
