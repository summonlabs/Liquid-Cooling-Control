#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "liquidcooling/ids.hpp"
#include "liquidcooling/status.hpp"
#include "liquidcooling/time.hpp"

namespace liquidcooling {

/// The set of operations a principal may perform.
///
/// The sets are deliberately not a linear privilege ladder.  A service
/// engineer holds service and isolation rights but never production actuation
/// rights, so service authority cannot accidentally inherit the ability to add
/// load or flow.  Isolation rights are the broadest because isolation reduces
/// hazard.
struct AuthorityCapabilities final {
    bool observe{false};
    bool isolate{false};
    bool production{false};
    bool service{false};

    [[nodiscard]] bool operator==(const AuthorityCapabilities&) const noexcept = default;

    [[nodiscard]] static constexpr AuthorityCapabilities none() noexcept { return AuthorityCapabilities{}; }

    [[nodiscard]] static constexpr AuthorityCapabilities auditor() noexcept {
        AuthorityCapabilities caps;
        caps.observe = true;
        return caps;
    }

    [[nodiscard]] static constexpr AuthorityCapabilities isolation_only() noexcept {
        AuthorityCapabilities caps;
        caps.observe = true;
        caps.isolate = true;
        return caps;
    }

    [[nodiscard]] static constexpr AuthorityCapabilities production_operator() noexcept {
        AuthorityCapabilities caps;
        caps.observe = true;
        caps.isolate = true;
        caps.production = true;
        return caps;
    }

    [[nodiscard]] static constexpr AuthorityCapabilities service_engineer() noexcept {
        AuthorityCapabilities caps;
        caps.observe = true;
        caps.isolate = true;
        caps.service = true;
        return caps;
    }

    [[nodiscard]] static constexpr AuthorityCapabilities full() noexcept {
        AuthorityCapabilities caps;
        caps.observe = true;
        caps.isolate = true;
        caps.production = true;
        caps.service = true;
        return caps;
    }

    /// True when every capability in the required set is held.
    [[nodiscard]] constexpr bool covers(const AuthorityCapabilities& required) const noexcept {
        if (required.observe && !observe) {
            return false;
        }
        if (required.isolate && !isolate) {
            return false;
        }
        if (required.production && !production) {
            return false;
        }
        if (required.service && !service) {
            return false;
        }
        return true;
    }
};

/// A configured principal allowed to request authority.
struct PrincipalRecord final {
    PrincipalId id{};
    std::string name{};
    AuthorityCapabilities capabilities{};
    bool enabled{false};
};

/// The configuration-owned registry of principals.
struct PrincipalRegistry final {
    ConfigGeneration generation{};
    std::vector<PrincipalRecord> principals{};

    [[nodiscard]] const PrincipalRecord* find(PrincipalId id) const noexcept;
    [[nodiscard]] Result<void> validate() const;
};

/// A scoped, expiring grant of authority bound to one controller incarnation.
///
/// A token issued by a previous incarnation, epoch, configuration generation or
/// topology generation is refused deterministically: it can never be replayed
/// against a restarted runtime.
struct AuthorityToken final {
    AuthorityTokenId id{};
    PrincipalId principal{};
    AuthorityCapabilities capabilities{};
    LoopId loop{};
    ControlPlaneEpoch epoch{};
    ControllerIncarnation incarnation{};
    ConfigGeneration config_generation{};
    TopologyGeneration topology_generation{};
    StateRevision revision_at_issue{};
    TimestampNs issued_at{};
    TimestampNs expires_at{};
    std::uint64_t nonce{0};

    [[nodiscard]] bool is_set() const noexcept { return id.valid(); }
};

/// Request to mint an authority token.
struct AuthorityRequest final {
    PrincipalId principal{};
    LoopId loop{};
    Duration lease{};
};

/// A token minted by this runtime plus the bookkeeping needed to fence it.
struct IssuedAuthority final {
    AuthorityTokenId id{};
    std::uint64_t nonce{0};
    PrincipalId principal{};
    TimestampNs expires_at{};
};

}  // namespace liquidcooling
