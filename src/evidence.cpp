#include "liquidcooling/evidence.hpp"

namespace liquidcooling {

StatusCode usability_status(EvidenceUsability usability) noexcept {
    switch (usability) {
        case EvidenceUsability::Usable:
            return StatusCode::Ok;
        case EvidenceUsability::Absent:
            return StatusCode::EvidenceAbsent;
        case EvidenceUsability::Stale:
            return StatusCode::EvidenceStale;
        case EvidenceUsability::Unsupported:
            return StatusCode::EvidenceUnsupported;
        case EvidenceUsability::Recovered:
            return StatusCode::RecoveredEvidenceRequiresRefresh;
        case EvidenceUsability::Future:
            return StatusCode::EvidenceFuture;
    }
    return StatusCode::InternalError;
}

}  // namespace liquidcooling
