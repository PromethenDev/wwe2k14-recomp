// WWE 2K14 native renderer startup safety (card G56 step A).
//
// Pure, SDK-independent decision and boundary helpers. The app layer supplies the
// live Vulkan-device flags and the blocking system-message callback; this header
// keeps the refusal policy and failed-startup path testable without a GPU or SDK.
#pragma once

#include <chrono>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace nr::startup {

// The owner-facing escape route must appear verbatim in every refusal message.
inline constexpr std::string_view kXboxRendererGuidance =
    "choose the Xbox renderer in the launcher";

// WWE2K14_NR=replace|shadow is the only native-renderer selector.
inline bool NativeModeSelected(const char* mode) {
  return mode && (std::strcmp(mode, "replace") == 0 || std::strcmp(mode, "shadow") == 0);
}

// WWE2K14_NR_DIRECT_PRESENT is an exact "1" switch, matching ConfigureNativeRenderer.
inline bool DirectPresentSelected(const char* value) {
  return value && std::strcmp(value, "1") == 0;
}

// The shared-device feature probe and constants32 startup choice below are Vulkan-only. The
// D3D12 direct-present path performs its own device adoption/capability checks in the backend.
inline bool ShouldSkipVulkanStartupGates(bool d3d12_backend, bool direct_present) {
  return d3d12_backend && direct_present;
}

// Filled from rex::graphics::NrVulkanDevice by the app. output_populated distinguishes a
// device that was reported (but lacks features) from an early GetNrVulkanDevice return.
struct SharedDeviceFeatures {
  bool output_populated = false;
  bool shader_int64 = false;
  bool buffer_device_address = false;
  bool descriptor_indexing = false;
};

struct StartupDecision {
  bool refuse = false;
  // Plain-language text shown to the owner; carries no feature, SDK or environment names.
  std::string message;
  // Technical cause for the log only. The app logs this before throwing the refusal, so the
  // blocking message never has to carry diagnostics.
  std::string log_detail;
};

// Thrown by Wwe2k14App::OnLoadXexImage and caught only by the startup boundary wrapper.
class NativeStartupRefusal : public std::exception {
 public:
  explicit NativeStartupRefusal(std::string message) : message_(std::move(message)) {}
  const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

// Generic owner-facing refusal. Technical diagnostics belong in StartupDecision::log_detail,
// never in the blocking UI text.
inline std::string RefusalMessage() {
  return "The Native renderer could not start. Please " + std::string(kXboxRendererGuidance) + ".";
}

// Technical list of the required device features that are absent, for logs only. The 32-bit
// constants ABI keeps Int64 out of the shaders, so shaderInt64 is not required there.
inline std::string MissingFeatureDetails(const SharedDeviceFeatures& features,
                                         bool constants32 = false) {
  std::string missing;
  auto add = [&missing](std::string_view name) {
    if (!missing.empty()) missing += ", ";
    missing += name;
  };
  if (!constants32 && !features.shader_int64) add("shaderInt64");
  if (!features.buffer_device_address) add("bufferDeviceAddress");
  if (!features.descriptor_indexing) add("descriptorIndexing");
  return missing;
}

// Owner-facing missing-feature message. Only the real graphics-chip name may appear in it,
// and it is omitted when the caller has none; feature names stay in MissingFeatureDetails.
inline std::string MissingFeaturesMessage(std::string_view graphics_chip_name = {}) {
  std::string message = "Your graphics chip";
  if (!graphics_chip_name.empty()) {
    message += " (";
    message += graphics_chip_name;
    message += ")";
  }
  message += " is missing a feature the Native renderer needs. Please ";
  message += kXboxRendererGuidance;
  message += ".";
  return message;
}

// Refuse only on the opted-in shared-device path. With no direct present the native
// backend creates its own device, so the shared-device feature gate must not block it.
inline StartupDecision DecideNativeSharedDevice(bool native_selected, bool direct_present,
                                                bool device_acquired,
                                                const SharedDeviceFeatures& features,
                                                bool constants32 = false,
                                                std::string_view graphics_chip_name = {}) {
  if (!native_selected || !direct_present) return {};
  // A reported device is required on every path. Populated handles never substitute for the
  // exported feature bits, even when the SDK reports the device as acquired.
  if (!features.output_populated) {
    return {true, RefusalMessage(),
            "the shared Vulkan device is unavailable: no device features were reported."};
  }
  const std::string missing = MissingFeatureDetails(features, constants32);
  // Buffer device address and descriptor indexing are required by both constants ABIs.
  if (!features.buffer_device_address || !features.descriptor_indexing) {
    return {true, MissingFeaturesMessage(graphics_chip_name),
            "the shared Vulkan device does not support required feature(s): " + missing + "."};
  }
  // The 32-bit constants ABI keeps Int64 out of the shaders, but only an acquired device may
  // take that path; the legacy ABI always needs shaderInt64.
  if (!device_acquired && constants32 && !features.shader_int64) {
    // A false SDK result must refuse even when the feature exports look usable. With the
    // constants32 opt-in and no Int64, the installed shared-device SDK is the blocker.
    return {true, RefusalMessage(),
            "this build cannot share the GPU with the installed renderer yet: the "
            "shared-device SDK does not provide the 32-bit constants mode."};
  }
  if (!features.shader_int64 && !(constants32 && device_acquired)) {
    return {true, MissingFeaturesMessage(graphics_chip_name),
            "the shared Vulkan device does not support required feature(s): " + missing + "."};
  }
  if (!device_acquired) {
    return {true, RefusalMessage(),
            "the shared Vulkan device could not be used: device acquisition failed."};
  }
  return {};
}

enum class ProbeStatus {
  kSucceeded,
  kTimedOut,
  kThrew,
};

template <typename T>
struct ProbeResult {
  ProbeStatus status = ProbeStatus::kTimedOut;
  std::optional<T> value;
  std::exception_ptr error;
};

// Run an SDK probe on the graphics command-processor thread and return a copy of its result.
// The callback owns only a shared promise and a by-value probe, so a callback arriving after
// the bounded wait is harmless. Probe exceptions are stored in the promise, never thrown back
// into the command-processor thread. An enqueue failure is reported like a probe failure.
template <typename Probe, typename Enqueue>
auto ProbeOnWorker(Probe&& probe, Enqueue&& enqueue, std::chrono::milliseconds timeout)
    -> ProbeResult<std::decay_t<std::invoke_result_t<Probe&>>> {
  using Result = std::decay_t<std::invoke_result_t<Probe&>>;
  auto promise = std::make_shared<std::promise<Result>>();
  std::future<Result> future = promise->get_future();
  try {
    enqueue([promise, probe = std::forward<Probe>(probe)]() mutable {
      try {
        promise->set_value(probe());
      } catch (...) {
        try {
          promise->set_exception(std::current_exception());
        } catch (...) {
          // The shared state may already be satisfied (late callback); nothing may escape.
        }
      }
    });
  } catch (...) {
    return {ProbeStatus::kThrew, std::nullopt, std::current_exception()};
  }
  if (future.wait_for(timeout) != std::future_status::ready) {
    return {ProbeStatus::kTimedOut, std::nullopt, nullptr};
  }
  ProbeResult<Result> result;
  result.status = ProbeStatus::kSucceeded;
  try {
    result.value.emplace(future.get());
  } catch (...) {
    result.status = ProbeStatus::kThrew;
    result.error = std::current_exception();
  }
  return result;
}

// Copy only the parsed positional options the entry point can supply. WindowedApp's
// parsed-argument map has no public enumerator, so the wrapper asks for each declared
// positional option by name and forwards the values it actually received.
template <typename GetArgument>
std::map<std::string, std::string> CopyPositionalArguments(
    const std::vector<std::string>& positional_options, GetArgument&& get_argument) {
  std::map<std::string, std::string> arguments;
  for (const std::string& option : positional_options) {
    if (std::optional<std::string> value = get_argument(option)) {
      arguments.emplace(option, std::move(*value));
    }
  }
  return arguments;
}

// Invoke the real app behind the boundary. Only a dedicated startup refusal is converted
// to a failed initialization; unrelated exceptions propagate unchanged.
template <typename InnerApp, typename Notify>
bool InitializeInnerApp(InnerApp& inner, std::map<std::string, std::string> arguments,
                        Notify&& notify) {
  inner.SetParsedArguments(std::move(arguments));
  try {
    return inner.OnInitialize();
  } catch (const NativeStartupRefusal& refusal) {
    notify(std::string_view(refusal.what()));
    return false;
  }
}

}  // namespace nr::startup
