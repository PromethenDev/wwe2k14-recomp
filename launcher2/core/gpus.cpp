#include "launcher_core.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

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

// Vulkan packs driverVersion per vendor: NVIDIA 10.8.8.6 bits, Intel on Windows 18.14 bits, others like apiVersion.
std::string DriverVersion(uint32_t vendor, uint32_t version) {
  if (vendor == 0x10DE) return std::to_string(version >> 22) + "." + std::to_string((version >> 14) & 0xFF);
#ifdef _WIN32
  if (vendor == 0x8086) return std::to_string(version >> 14) + "." + std::to_string(version & 0x3FFF);
#endif
  return std::to_string(VK_API_VERSION_MAJOR(version)) + "." + std::to_string(VK_API_VERSION_MINOR(version)) + "." +
         std::to_string(VK_API_VERSION_PATCH(version));
}

struct RecommendationProfile {
  std::string_view vendor_prefix;
  std::string_view name_pattern;
  uint64_t min_vram_mb;
  uint64_t max_vram_mb;
  unsigned min_logical_processors;
  unsigned max_logical_processors;
  PictureSize picture_size;
  FrameRate frame_rate;
  std::string_view reason;
};

// These profiles are deliberately limited to measured GPU classes. No CPU- or VRAM-based split has enough
// comparable evidence yet, so those ranges stay open while remaining explicit inputs to the table matcher.
constexpr RecommendationProfile kMeasuredProfiles[] = {
    {"8086:", "uhd graphics 630", 0, std::numeric_limits<uint64_t>::max(), 1,
     std::numeric_limits<unsigned>::max(), PictureSize::k480p, FrameRate::k60Everywhere,
     "480p picture, 60 fps everywhere"},  // owner goal 10-23: UHD 630 locked 480p 60 (fixed preset, not auto-switching)
    {"1002:", "radeon 780m", 0, std::numeric_limits<uint64_t>::max(), 1,
     std::numeric_limits<unsigned>::max(), PictureSize::k720p, FrameRate::kStandard30_60,
     "720p picture, 60 fps in matches"},
    // Owner laptop RTX 4060 (2026-10-20, 1020-TEST launcher 8): 1440p, 60 everywhere -> entrance 59.4 fps
    // (on-grid60 95.7 %), match 59.1 (98.2 %); repeat run 59.5 / 58.4, freeze 0. Owner: "great".
    {"10de:", "rtx 4060", 0, std::numeric_limits<uint64_t>::max(), 1,
     std::numeric_limits<unsigned>::max(), PictureSize::k1440p, FrameRate::k60Everywhere,
     "1440p picture, 60 fps everywhere"},
};

unsigned LogicalProcessorCount(unsigned requested) {
  if (requested != 0) return requested;
  const unsigned detected = std::thread::hardware_concurrency();
  return detected == 0 ? 1 : detected;
}

bool Matches(const GpuInfo& gpu, const RecommendationProfile& profile, unsigned logical_processors) {
  const std::string id = Lower(gpu.id);
  const std::string name = Lower(gpu.name);
  return id.starts_with(profile.vendor_prefix) && name.find(profile.name_pattern) != std::string::npos &&
         gpu.vram_mb >= profile.min_vram_mb && gpu.vram_mb <= profile.max_vram_mb &&
         logical_processors >= profile.min_logical_processors &&
         logical_processors <= profile.max_logical_processors;
}

bool IsManual(const Settings& settings, RecommendedField field) {
  if (!settings.recommendation_fields_tracked) return settings.explicit_choice;
  switch (field) {
    case RecommendedField::kResolution: return settings.manual_resolution;
    case RecommendedField::kPictureSize: return settings.manual_picture_size;
    case RecommendedField::kFrameRate: return settings.manual_frame_rate;
    case RecommendedField::kXboxFrameRate: return settings.manual_xbox_frame_rate;
  }
  return false;
}

std::string ResolutionText(Resolution value) {
  switch (value) {
    case Resolution::k1080p: return "1080p";
    case Resolution::k1440p: return "1440p";
    case Resolution::k720p: default: return "720p";
  }
}

std::string PictureSizeText(PictureSize value) {
  switch (value) {
    case PictureSize::k360p: return "360p";
    case PictureSize::k480p: return "480p";
    case PictureSize::k540p: return "540p";
    case PictureSize::k1080p: return "1080p";
    case PictureSize::k1440p: return "1440p";
    case PictureSize::k4k: return "4K";
    case PictureSize::k720p: default: return "720p";
  }
}

std::string FrameRateText(FrameRate value) {
  switch (value) {
    case FrameRate::kLocked30: return "30 fps everywhere";
    case FrameRate::k60Everywhere: return "60 fps everywhere";
    case FrameRate::kStandard30_60: default: return "60 fps in matches, 30 fps in entrances";
  }
}

void AddChange(std::vector<RecommendationChange>& changes, const Settings& current, RecommendedField field,
               std::string label, std::string current_value, std::string recommended_value) {
  if (current_value == recommended_value) return;
  changes.push_back({std::move(label), std::move(current_value), std::move(recommended_value), IsManual(current, field)});
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
        GpuInfo info{identity, name, properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
                     static_cast<int>(index)};
        info.driver = DriverVersion(properties.vendorID, properties.driverVersion);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(device, &memory);
        for (uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap) {
          if (memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            info.vram_mb = std::max<uint64_t>(info.vram_mb, memory.memoryHeaps[heap].size >> 20);
          }
        }
        result.push_back(std::move(info));
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

Recommendation RecommendedSettings(const std::vector<GpuInfo>& gpus, unsigned logical_processors) {
  Recommendation recommendation;
  recommendation.settings.resolution = Resolution::k720p;
  recommendation.settings.explicit_choice = false;
  logical_processors = LogicalProcessorCount(logical_processors);
  // The strongest card: the first discrete one, otherwise the first card (an iGPU-only PC).
  const auto discrete = std::find_if(gpus.begin(), gpus.end(), [](const GpuInfo& gpu) {
    return !gpu.integrated;
  });
  if (!gpus.empty()) {
    const GpuInfo& best = discrete != gpus.end() ? *discrete : gpus.front();
    recommendation.gpu_id = best.id;
    if (!best.integrated) {
      recommendation.reason = best.name + " - discrete graphics";
      recommendation.settings.picture_size = PictureSize::k720p;
      recommendation.settings.frame_rate = FrameRate::kStandard30_60;
    } else {
      recommendation.reason = best.name + " - integrated graphics";
      // Keep the previous conservative integrated-GPU fallback until a device class has measured evidence.
      recommendation.settings.picture_size = PictureSize::k480p;
      recommendation.settings.frame_rate = FrameRate::kLocked30;
    }
    for (const RecommendationProfile& profile : kMeasuredProfiles) {
      if (!Matches(best, profile, logical_processors)) continue;
      recommendation.reason = best.name + " - " + std::string(profile.reason);
      recommendation.settings.picture_size = profile.picture_size;
      recommendation.settings.frame_rate = profile.frame_rate;
      break;
    }
  } else {
    recommendation.reason = "Automatic graphics settings";
  }
  return recommendation;
}

Recommendation RecommendedSettingsFor(const std::vector<GpuInfo>& gpus, const std::string& gpu_id,
                                       unsigned logical_processors) {
  if (!gpu_id.empty()) {
    const auto selected = std::find_if(gpus.begin(), gpus.end(), [&](const GpuInfo& gpu) { return gpu.id == gpu_id; });
    if (selected != gpus.end()) return RecommendedSettings({*selected}, logical_processors);
  }
  return RecommendedSettings(gpus, logical_processors);
}

Settings ApplyRecommendedSettings(const Settings& current, const Settings& recommended, bool replace_manual) {
  Settings result = current;
  if (replace_manual || !IsManual(current, RecommendedField::kResolution)) result.resolution = recommended.resolution;
  if (replace_manual || !IsManual(current, RecommendedField::kPictureSize)) {
    result.picture_size = recommended.picture_size;
  }
  if (replace_manual || !IsManual(current, RecommendedField::kFrameRate)) result.frame_rate = recommended.frame_rate;
  if (replace_manual || !IsManual(current, RecommendedField::kXboxFrameRate)) {
    result.xbox_frame_rate = recommended.xbox_frame_rate;
  }
  if (replace_manual) {
    result.recommendation_fields_tracked = true;
    result.manual_resolution = false;
    result.manual_picture_size = false;
    result.manual_frame_rate = false;
    result.manual_xbox_frame_rate = false;
    result.explicit_choice = false;
  }
  return result;
}

std::vector<RecommendationChange> CompareRecommendedSettings(const Settings& current, const Settings& recommended) {
  std::vector<RecommendationChange> changes;
  AddChange(changes, current, RecommendedField::kResolution, "Output resolution",
            ResolutionText(current.resolution), ResolutionText(recommended.resolution));
  AddChange(changes, current, RecommendedField::kPictureSize, "3D picture size",
            PictureSizeText(current.picture_size), PictureSizeText(recommended.picture_size));
  AddChange(changes, current, RecommendedField::kFrameRate, "Native frame rate",
            FrameRateText(current.frame_rate), FrameRateText(recommended.frame_rate));
  AddChange(changes, current, RecommendedField::kXboxFrameRate, "Xbox frame rate",
            FrameRateText(current.xbox_frame_rate), FrameRateText(recommended.xbox_frame_rate));
  return changes;
}

}  // namespace wwe2k14::launcher
