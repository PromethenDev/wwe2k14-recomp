#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "plume_render_interface.h"

namespace wwe2k14::plume_backend {

enum class DeviceSelectionError {
    none,
    empty_preferred_name,
    name_not_found,
    duplicate_name,
};

struct DeviceSelection {
    DeviceSelectionError error = DeviceSelectionError::none;
    std::string exact_name;

    explicit operator bool() const noexcept { return error == DeviceSelectionError::none; }
};

// CPU-only validation. Plume's createDevice(name) treats a name as a preference
// and can otherwise select a device by type; this helper requires one exact match.
DeviceSelection select_unique_device_name(
    const std::vector<std::string>& available_names,
    std::string_view preferred_name);

class Backend final {
public:
    Backend() = default;
    ~Backend() = default;

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    Backend(Backend&&) = delete;
    Backend& operator=(Backend&&) = delete;

    // The only entrypoint that starts Plume/Vulkan. Construction is inert.
    bool initialize(const std::string& preferred_device_name, std::string* error = nullptr);

    bool initialized() const noexcept;
    plume::RenderInterface* interface() noexcept { return interface_.get(); }
    const plume::RenderInterface* interface() const noexcept { return interface_.get(); }
    plume::RenderDevice* device() noexcept { return device_.get(); }
    const plume::RenderDevice* device() const noexcept { return device_.get(); }
    plume::RenderCommandQueue* graphics_queue() noexcept { return graphics_queue_.get(); }
    const plume::RenderCommandQueue* graphics_queue() const noexcept { return graphics_queue_.get(); }

private:
    // Declaration order is intentional: reverse member destruction is queue,
    // device, interface, preserving Plume's parent lifetimes.
    std::unique_ptr<plume::RenderInterface> interface_;
    std::unique_ptr<plume::RenderDevice> device_;
    std::unique_ptr<plume::RenderCommandQueue> graphics_queue_;
};

} // namespace wwe2k14::plume_backend
