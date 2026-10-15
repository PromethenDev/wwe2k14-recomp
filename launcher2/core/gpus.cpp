#include "launcher_core.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <string>

#include <volk.h>

namespace wwe2k14::launcher {
namespace {

std::string DeviceId(uint32_t vendor, uint32_t device) {
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), "%04X:%04X", vendor, device);
  return buffer;
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

}  // namespace

std::vector<GpuInfo> ListGpus() {
  std::vector<GpuInfo> result;
  if (volkInitialize() != VK_SUCCESS) return result;

  VkApplicationInfo application{};
  application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application.pApplicationName = "WWE 2K14 PC Recompiled Launcher";
  application.applicationVersion = 1;
  application.pEngineName = "WWE 2K14 PC Recompiled Launcher";
  application.engineVersion = 1;
  application.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &application;

  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS || instance == VK_NULL_HANDLE) {
    volkFinalize();
    return result;
  }
  volkLoadInstance(instance);

  uint32_t device_count = 0;
  VkResult enumerate_result = vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
  if (enumerate_result == VK_SUCCESS && device_count != 0) {
    std::vector<VkPhysicalDevice> devices(device_count);
    enumerate_result = vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
    if (enumerate_result == VK_SUCCESS || enumerate_result == VK_INCOMPLETE) {
      devices.resize(device_count);
      std::map<std::string, unsigned> identity_counts;
      for (size_t index = 0; index < devices.size(); ++index) {
        const VkPhysicalDevice device = devices[index];
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        const std::string name = properties.deviceName[0] ? properties.deviceName : "Vulkan GPU";
        const std::string lowered = Lower(name);
        if (lowered.find("llvmpipe") != std::string::npos ||
            lowered.find("lavapipe") != std::string::npos ||
            lowered.find("software rasterizer") != std::string::npos) {
          continue;
        }
        const std::string identity = DeviceId(properties.vendorID, properties.deviceID);
        ++identity_counts[identity];
        result.push_back({identity, name,
                          properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
                          static_cast<int>(index)});
      }
      for (auto& gpu : result) {
        if (identity_counts[gpu.id] > 1) gpu.id += ":" + std::to_string(gpu.vulkan_index);
      }
    }
  }

  vkDestroyInstance(instance, nullptr);
  volkFinalize();
  return result;
}

Recommendation RecommendedSettings(const std::vector<GpuInfo>& gpus) {
  Recommendation recommendation;
  recommendation.settings.resolution = Resolution::k720p;
  recommendation.settings.explicit_choice = false;
  const auto discrete = std::find_if(gpus.begin(), gpus.end(), [](const GpuInfo& gpu) {
    return !gpu.integrated;
  });
  if (discrete != gpus.end()) {
    recommendation.reason = discrete->name + " - discrete graphics";
  } else if (!gpus.empty()) {
    recommendation.reason = gpus.front().name + " - integrated graphics";
  } else {
    recommendation.reason = "Automatic graphics settings";
  }
  return recommendation;
}

}  // namespace wwe2k14::launcher
