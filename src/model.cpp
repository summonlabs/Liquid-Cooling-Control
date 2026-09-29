#include "liquidcooling/model.hpp"

namespace liquidcooling {

const DeviceRecord* LoopRecord::find_device(DeviceId device_id) const noexcept {
    for (const auto& device : devices) {
        if (device.id == device_id) {
            return &device;
        }
    }
    return nullptr;
}

DeviceRecord* LoopRecord::find_device(DeviceId device_id) noexcept {
    for (auto& device : devices) {
        if (device.id == device_id) {
            return &device;
        }
    }
    return nullptr;
}

}  // namespace liquidcooling
