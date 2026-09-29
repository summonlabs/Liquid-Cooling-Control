#include "liquidcooling/adapter.hpp"

namespace liquidcooling {

StatusCode to_status(AdapterErrorCode error) noexcept {
    switch (error) {
        case AdapterErrorCode::None:
            return StatusCode::Ok;
        case AdapterErrorCode::Unavailable:
            return StatusCode::AdapterUnavailable;
        case AdapterErrorCode::DeviceMissing:
            return StatusCode::AdapterDeviceMissing;
        case AdapterErrorCode::GenerationMismatch:
            return StatusCode::AdapterGenerationMismatch;
        case AdapterErrorCode::Rejected:
            return StatusCode::AdapterRejected;
        case AdapterErrorCode::Busy:
            return StatusCode::AdapterBusy;
        case AdapterErrorCode::Internal:
            return StatusCode::AdapterInternalError;
        case AdapterErrorCode::Unsupported:
            return StatusCode::EvidenceUnsupported;
    }
    return StatusCode::AdapterInternalError;
}

ILoopAdapter::~ILoopAdapter() = default;

}  // namespace liquidcooling
