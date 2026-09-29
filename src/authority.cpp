#include "liquidcooling/authority.hpp"

#include <algorithm>

namespace liquidcooling {

const PrincipalRecord* PrincipalRegistry::find(PrincipalId id) const noexcept {
    for (const auto& principal : principals) {
        if (principal.id == id) {
            return &principal;
        }
    }
    return nullptr;
}

Result<void> PrincipalRegistry::validate() const {
    if (principals.size() > 1024) {
        return Status{StatusCode::TooManyObjects, "principal registry exceeds 1024 entries"};
    }
    if (!generation.valid()) {
        return Status{StatusCode::MissingRequiredField, "principal registry generation must be non-zero"};
    }
    std::vector<PrincipalId> seen;
    seen.reserve(principals.size());
    for (const auto& principal : principals) {
        if (!principal.id.valid()) {
            return Status{StatusCode::MissingRequiredField, "principal id must be non-zero"};
        }
        const auto name = validate_name(principal.name, "principal name");
        if (!name.ok()) {
            return name.status();
        }
        if (std::find(seen.begin(), seen.end(), principal.id) != seen.end()) {
            return Status{StatusCode::DuplicateIdentity, "duplicate principal id in registry"};
        }
        seen.push_back(principal.id);
    }
    return ok_result();
}

}  // namespace liquidcooling
