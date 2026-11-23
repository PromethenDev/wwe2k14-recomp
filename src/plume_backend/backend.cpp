#include "backend.h"

#include <algorithm>

#include "plume_vulkan.h"

namespace plume {
// The pinned Plume source defines this factory but does not declare it in a
// public header. This declaration matches its no-SDL Vulkan implementation.
std::unique_ptr<RenderInterface> CreateVulkanInterface();
} // namespace plume

namespace wwe2k14::plume_backend {

DeviceSelection select_unique_device_name(
    const std::vector<std::string>& available_names,
    std::string_view preferred_name) {
    if (preferred_name.empty()) {
        return {DeviceSelectionError::empty_preferred_name, {}};
    }

    const auto count = std::count(available_names.begin(), available_names.end(), preferred_name);
    if (count == 0) {
        return {DeviceSelectionError::name_not_found, {}};
    }
    if (count != 1) {
        return {DeviceSelectionError::duplicate_name, {}};
    }
    return {DeviceSelectionError::none, std::string(preferred_name)};
}

bool Backend::initialize(const std::string& preferred_device_name, std::string* error) {
    // Reinitialization first tears down dependents before their parents.
    graphics_queue_.reset();
    device_.reset();
    interface_.reset();

    auto fail = [error](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    if (preferred_device_name.empty()) {
        return fail("preferred Vulkan device name must not be empty");
    }

    interface_ = plume::CreateVulkanInterface();
    if (!interface_) {
        return fail("Plume Vulkan interface creation failed");
    }

    const DeviceSelection selection =
        select_unique_device_name(interface_->getDeviceNames(), preferred_device_name);
    switch (selection.error) {
    case DeviceSelectionError::none:
        break;
    case DeviceSelectionError::empty_preferred_name:
        interface_.reset();
        return fail("preferred Vulkan device name must not be empty");
    case DeviceSelectionError::name_not_found:
        interface_.reset();
        return fail("preferred Vulkan device name was not enumerated");
    case DeviceSelectionError::duplicate_name:
        interface_.reset();
        return fail("preferred Vulkan device name is ambiguous");
    }

    device_ = interface_->createDevice(selection.exact_name);
    if (!device_) {
        interface_.reset();
        return fail("Plume failed to create the uniquely selected device");
    }
    if (device_->getDescription().name != selection.exact_name) {
        device_.reset();
        interface_.reset();
        return fail("Plume returned a device other than the exact preferred name");
    }

    // Plume's RenderCommandListType::DIRECT is its graphics-capable direct queue;
    // this pinned API has no enum member named GRAPHICS.
    graphics_queue_ = device_->createCommandQueue(plume::RenderCommandListType::DIRECT);
    if (!graphics_queue_) {
        device_.reset();
        interface_.reset();
        return fail("Plume failed to create the direct/graphics queue");
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool Backend::initialized() const noexcept {
    return interface_ != nullptr && device_ != nullptr && graphics_queue_ != nullptr;
}

} // namespace wwe2k14::plume_backend
