// Card G56 CPU-only test for the native startup safety helpers (native/nr/nr_startup.h).
//
// No SDK, GPU, game or window: it pins the refusal policy, the exact owner guidance, the
// argument forwarding of the boundary wrapper's helper, and the failed-startup contract
// (a dedicated refusal becomes false plus exactly one notification; anything else
// propagates unchanged).
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "native/nr/nr_startup.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
  }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

nr::startup::SharedDeviceFeatures AllFeatures() {
  nr::startup::SharedDeviceFeatures features;
  features.output_populated = true;
  features.shader_int64 = true;
  features.buffer_device_address = true;
  features.descriptor_indexing = true;
  return features;
}

// Stands in for Wwe2k14App behind the boundary: records what the wrapper forwards and
// fails the way the real app does.
struct FakeInner {
  std::map<std::string, std::string> received;
  bool destroy_called = false;
  bool initialize_result = true;
  bool throw_refusal = false;
  bool throw_other = false;
  std::string refusal_text = "refused by fake inner";

  void SetParsedArguments(std::map<std::string, std::string> args) { received = std::move(args); }

  bool OnInitialize() {
    if (throw_refusal) throw nr::startup::NativeStartupRefusal(refusal_text);
    if (throw_other) throw std::runtime_error("unrelated failure");
    return initialize_result;
  }
};

void TestSelectorPermutations() {
  CHECK(!nr::startup::NativeModeSelected(nullptr));
  CHECK(!nr::startup::NativeModeSelected(""));
  CHECK(!nr::startup::NativeModeSelected("0"));
  CHECK(!nr::startup::NativeModeSelected("Replace"));
  CHECK(!nr::startup::NativeModeSelected("replace "));
  CHECK(nr::startup::NativeModeSelected("replace"));
  CHECK(nr::startup::NativeModeSelected("shadow"));

  CHECK(!nr::startup::DirectPresentSelected(nullptr));
  CHECK(!nr::startup::DirectPresentSelected(""));
  CHECK(!nr::startup::DirectPresentSelected("0"));
  CHECK(!nr::startup::DirectPresentSelected("true"));
  CHECK(!nr::startup::DirectPresentSelected(" 1"));
  CHECK(nr::startup::DirectPresentSelected("1"));
}

void TestBackendStartupGateSelection() {
  using nr::startup::ShouldSkipVulkanStartupGates;

  // Vulkan keeps the existing startup decisions, regardless of direct-present selection.
  CHECK(!ShouldSkipVulkanStartupGates(false, false));
  CHECK(!ShouldSkipVulkanStartupGates(false, true));
  // D3D12 without shared direct present still takes its own-device constants ABI validation.
  CHECK(!ShouldSkipVulkanStartupGates(true, false));
  // Only D3D12 direct present bypasses both Vulkan-only startup gates.
  CHECK(ShouldSkipVulkanStartupGates(true, true));
}

void TestGuidanceIsVerbatim() {
  CHECK(nr::startup::kXboxRendererGuidance == "choose the Xbox renderer in the launcher");
  const std::string plain = nr::startup::RefusalMessage();
  CHECK(plain ==
        "The Native renderer could not start. Please choose the Xbox renderer in the launcher.");
  CHECK(Contains(plain, nr::startup::kXboxRendererGuidance));

  const std::string unnamed = nr::startup::MissingFeaturesMessage();
  CHECK(unnamed ==
        "Your graphics chip is missing a feature the Native renderer needs. Please "
        "choose the Xbox renderer in the launcher.");
  CHECK(Contains(unnamed, nr::startup::kXboxRendererGuidance));

  const std::string named = nr::startup::MissingFeaturesMessage("Intel UHD Graphics 630");
  CHECK(named ==
        "Your graphics chip (Intel UHD Graphics 630) is missing a feature the Native "
        "renderer needs. Please choose the Xbox renderer in the launcher.");
  CHECK(Contains(named, "Intel UHD Graphics 630"));
}

// The blocking UI must never carry feature, SDK or environment/switch names.
void CheckPlainUiText(const std::string& message) {
  CHECK(Contains(message, nr::startup::kXboxRendererGuidance));
  CHECK(!Contains(message, "Vulkan"));
  CHECK(!Contains(message, "vulkan"));
  CHECK(!Contains(message, "shaderInt64"));
  CHECK(!Contains(message, "bufferDeviceAddress"));
  CHECK(!Contains(message, "descriptorIndexing"));
  CHECK(!Contains(message, "SDK"));
  CHECK(!Contains(message, "constants"));
  CHECK(!Contains(message, "WWE2K14_"));
  CHECK(!Contains(message, "Native renderer startup refused"));
}

void TestUiTextIsPlainLanguage() {
  CheckPlainUiText(nr::startup::RefusalMessage());
  CheckPlainUiText(nr::startup::MissingFeaturesMessage());
  CheckPlainUiText(nr::startup::MissingFeaturesMessage("Intel UHD Graphics 630"));
}


void TestMissingFeatureDetails() {
  const nr::startup::SharedDeviceFeatures none;
  CHECK(nr::startup::MissingFeatureDetails(none) ==
        "shaderInt64, bufferDeviceAddress, descriptorIndexing");
  // The 32-bit constants ABI keeps Int64 out of the shaders, so it is not required there.
  CHECK(nr::startup::MissingFeatureDetails(none, true) ==
        "bufferDeviceAddress, descriptorIndexing");
  CHECK(nr::startup::MissingFeatureDetails(AllFeatures()).empty());
  CHECK(nr::startup::MissingFeatureDetails(AllFeatures(), true).empty());

  nr::startup::SharedDeviceFeatures only_int64 = AllFeatures();
  only_int64.shader_int64 = false;
  CHECK(nr::startup::MissingFeatureDetails(only_int64) == "shaderInt64");
  CHECK(nr::startup::MissingFeatureDetails(only_int64, true).empty());

  nr::startup::SharedDeviceFeatures only_bda = AllFeatures();
  only_bda.buffer_device_address = false;
  CHECK(nr::startup::MissingFeatureDetails(only_bda) == "bufferDeviceAddress");
  CHECK(nr::startup::MissingFeatureDetails(only_bda, true) == "bufferDeviceAddress");

  nr::startup::SharedDeviceFeatures only_di = AllFeatures();
  only_di.descriptor_indexing = false;
  CHECK(nr::startup::MissingFeatureDetails(only_di) == "descriptorIndexing");
  CHECK(nr::startup::MissingFeatureDetails(only_di, true) == "descriptorIndexing");
}

void TestDecisionPermutations() {
  using nr::startup::DecideNativeSharedDevice;
  const nr::startup::SharedDeviceFeatures good = AllFeatures();
  nr::startup::SharedDeviceFeatures unpopulated;
  unpopulated.shader_int64 = true;  // flags must not matter without a reported device
  const std::string generic = nr::startup::RefusalMessage();
  const std::string chip_needed = nr::startup::MissingFeaturesMessage();
  const std::string chip_named = nr::startup::MissingFeaturesMessage("Intel UHD Graphics 630");

  // Not opted in, or not on the shared-device path: never refuse, whatever the device says.
  CHECK(!DecideNativeSharedDevice(false, false, false, unpopulated).refuse);
  CHECK(!DecideNativeSharedDevice(false, true, false, unpopulated).refuse);
  CHECK(!DecideNativeSharedDevice(true, false, false, unpopulated).refuse);
  CHECK(!DecideNativeSharedDevice(true, false, false, good).refuse);
  const nr::startup::StartupDecision not_opted = DecideNativeSharedDevice(true, false, false, good);
  CHECK(not_opted.message.empty());
  CHECK(not_opted.log_detail.empty());

  // A successfully acquired shared device: allow the legacy ABI, with no diagnostics.
  const nr::startup::StartupDecision allowed = DecideNativeSharedDevice(true, true, true, good);
  CHECK(!allowed.refuse);
  CHECK(allowed.message.empty());
  CHECK(allowed.log_detail.empty());

  // Acquired handles never substitute for exported features: refuse incomplete exports.
  const nr::startup::StartupDecision unpopulated_acquired =
      DecideNativeSharedDevice(true, true, true, unpopulated);
  CHECK(unpopulated_acquired.refuse);
  CHECK(unpopulated_acquired.message == generic);
  CheckPlainUiText(unpopulated_acquired.message);
  CHECK(Contains(unpopulated_acquired.log_detail, "unavailable"));

  nr::startup::SharedDeviceFeatures acquired_no_bda = AllFeatures();
  acquired_no_bda.buffer_device_address = false;
  const nr::startup::StartupDecision no_bda =
      DecideNativeSharedDevice(true, true, true, acquired_no_bda);
  CHECK(no_bda.refuse);
  CHECK(no_bda.message == chip_needed);
  CheckPlainUiText(no_bda.message);
  CHECK(Contains(no_bda.log_detail, "required feature(s): bufferDeviceAddress."));

  // The graphics chip name appears only in the chip-feature message, never in log_detail.
  const nr::startup::StartupDecision no_bda_named =
      DecideNativeSharedDevice(true, true, true, acquired_no_bda, false, "Intel UHD Graphics 630");
  CHECK(no_bda_named.refuse);
  CHECK(no_bda_named.message == chip_named);
  CHECK(Contains(no_bda_named.message, "Intel UHD Graphics 630"));
  CHECK(no_bda_named.log_detail == no_bda.log_detail);
  CHECK(!Contains(no_bda_named.log_detail, "Intel UHD Graphics 630"));

  nr::startup::SharedDeviceFeatures acquired_no_di = AllFeatures();
  acquired_no_di.descriptor_indexing = false;
  const nr::startup::StartupDecision no_di =
      DecideNativeSharedDevice(true, true, true, acquired_no_di);
  CHECK(no_di.refuse);
  CHECK(no_di.message == chip_needed);
  CHECK(Contains(no_di.log_detail, "required feature(s): descriptorIndexing."));

  // No Int64: allowed only with the validated constants32 opt-in and an acquired device.
  nr::startup::SharedDeviceFeatures no_int64 = AllFeatures();
  no_int64.shader_int64 = false;
  const nr::startup::StartupDecision legacy_no_int64 =
      DecideNativeSharedDevice(true, true, true, no_int64);
  CHECK(legacy_no_int64.refuse);
  CHECK(legacy_no_int64.message == chip_needed);
  CHECK(Contains(legacy_no_int64.log_detail, "required feature(s): shaderInt64."));
  CHECK(!DecideNativeSharedDevice(true, true, true, no_int64, true).refuse);

  // constants32 never bypasses buffer device address or descriptor indexing, and never lists
  // shaderInt64 as required.
  nr::startup::SharedDeviceFeatures constants_no_bda = no_int64;
  constants_no_bda.buffer_device_address = false;
  const nr::startup::StartupDecision constants_bda =
      DecideNativeSharedDevice(true, true, true, constants_no_bda, true);
  CHECK(constants_bda.refuse);
  CHECK(constants_bda.message == chip_needed);
  CHECK(Contains(constants_bda.log_detail, "required feature(s): bufferDeviceAddress."));
  CHECK(!Contains(constants_bda.log_detail, "shaderInt64"));
  nr::startup::SharedDeviceFeatures constants_no_di = no_int64;
  constants_no_di.descriptor_indexing = false;
  const nr::startup::StartupDecision constants_di =
      DecideNativeSharedDevice(true, true, true, constants_no_di, true);
  CHECK(constants_di.refuse);
  CHECK(Contains(constants_di.log_detail, "required feature(s): descriptorIndexing."));
  CHECK(!Contains(constants_di.log_detail, "shaderInt64"));

  // A false SDK acquisition must refuse even when the features look usable.
  const nr::startup::StartupDecision old_sdk_legacy =
      DecideNativeSharedDevice(true, true, false, good);
  CHECK(old_sdk_legacy.refuse);
  CHECK(old_sdk_legacy.message == generic);
  CheckPlainUiText(old_sdk_legacy.message);
  CHECK(Contains(old_sdk_legacy.log_detail, "could not be used"));

  // The installed-SDK constants-mode refusal keeps its technical cause in the log only.
  const nr::startup::StartupDecision old_sdk_constants =
      DecideNativeSharedDevice(true, true, false, no_int64, true);
  CHECK(old_sdk_constants.refuse);
  CHECK(old_sdk_constants.message == generic);
  CheckPlainUiText(old_sdk_constants.message);
  CHECK(Contains(old_sdk_constants.log_detail, "32-bit constants mode"));
  CHECK(!Contains(old_sdk_constants.message, "32-bit"));

  // A chip name never turns an availability/acquisition refusal into a feature message.
  CHECK(DecideNativeSharedDevice(true, true, false, no_int64, true, "Intel UHD Graphics 630")
            .message == generic);
  CHECK(DecideNativeSharedDevice(true, true, false, unpopulated, false, "Intel UHD Graphics 630")
            .message == generic);
  CHECK(DecideNativeSharedDevice(true, true, false, unpopulated, true).refuse);

  // Populated output missing a feature, device not acquired: chip message plus the list.
  nr::startup::SharedDeviceFeatures missing = AllFeatures();
  missing.buffer_device_address = false;
  const nr::startup::StartupDecision decision =
      DecideNativeSharedDevice(true, true, false, missing);
  CHECK(decision.refuse);
  CHECK(decision.message == chip_needed);
  CHECK(Contains(decision.log_detail, "does not support required feature(s): bufferDeviceAddress."));
  CheckPlainUiText(decision.message);

  // Nothing reported at all: refuse as unavailable.
  const nr::startup::StartupDecision unavailable =
      DecideNativeSharedDevice(true, true, false, unpopulated);
  CHECK(unavailable.refuse);
  CHECK(unavailable.message == generic);
  CheckPlainUiText(unavailable.message);
  CHECK(!unavailable.log_detail.empty());

  // Fully populated, every feature on, yet not acquired: still a clean refusal, never a launch.
  const nr::startup::StartupDecision diverged =
      DecideNativeSharedDevice(true, true, false, good);
  CHECK(diverged.refuse);
  CHECK(diverged.message == generic);
  CheckPlainUiText(diverged.message);
  CHECK(Contains(diverged.log_detail, "the shared Vulkan device could not be used"));
}


void TestCopyPositionalArguments() {
  const std::vector<std::string> declared = {"disc_image", "game_data", "unused"};
  const std::map<std::string, std::string> available = {{"disc_image", "/tmp/disc"},
                                                        {"game_data", "/tmp/data"}};
  int asked = 0;
  const std::map<std::string, std::string> copied = nr::startup::CopyPositionalArguments(
      declared, [&](const std::string& name) {
        ++asked;
        const auto it = available.find(name);
        if (it == available.end()) return std::optional<std::string>{};
        return std::optional<std::string>{it->second};
      });
  CHECK(asked == 3);
  CHECK(copied.size() == 2);
  CHECK(copied.at("disc_image") == "/tmp/disc");
  CHECK(copied.at("game_data") == "/tmp/data");
  CHECK(copied.find("unused") == copied.end());

  // Empty declaration asks nothing.
  const std::map<std::string, std::string> empty =
      nr::startup::CopyPositionalArguments({}, [](const std::string&) {
        return std::optional<std::string>{"must not be asked"};
      });
  CHECK(empty.empty());
}

void TestInitializeForwardsArgumentsAndResult() {
  FakeInner inner;
  const std::map<std::string, std::string> arguments = {{"disc_image", "/tmp/disc"}};
  int notified = 0;
  const bool ok = nr::startup::InitializeInnerApp(inner, arguments, [&](std::string_view) { ++notified; });
  CHECK(ok);
  CHECK(notified == 0);
  CHECK(inner.received == arguments);

  FakeInner failing;
  failing.initialize_result = false;
  notified = 0;
  const bool failed = nr::startup::InitializeInnerApp(failing, {}, [&](std::string_view) { ++notified; });
  CHECK(!failed);
  CHECK(notified == 0);  // a plain false is not a refusal: no message box
}

void TestRefusalBecomesOneNotification() {
  FakeInner inner;
  inner.throw_refusal = true;
  inner.refusal_text = nr::startup::RefusalMessage();
  std::vector<std::string> notifications;
  const bool ok = nr::startup::InitializeInnerApp(inner, {}, [&](std::string_view message) {
    notifications.emplace_back(message);
  });
  CHECK(!ok);
  CHECK(notifications.size() == 1);
  CHECK(notifications[0] == inner.refusal_text);
  CHECK(Contains(notifications[0], nr::startup::kXboxRendererGuidance));
}

void TestUnrelatedExceptionPropagates() {
  FakeInner inner;
  inner.throw_other = true;
  int notified = 0;
  bool propagated = false;
  try {
    nr::startup::InitializeInnerApp(inner, {}, [&](std::string_view) { ++notified; });
  } catch (const std::runtime_error& error) {
    propagated = std::string_view(error.what()) == "unrelated failure";
  }
  CHECK(propagated);
  CHECK(notified == 0);  // only a NativeStartupRefusal may produce the blocking message
}

// Run ProbeOnWorker while this thread plays the command processor. The callback is queued
// by the worker and invoked from here after the queue gate opens.
template <typename Probe>
auto RunQueuedProbe(Probe probe, std::chrono::milliseconds timeout,
                    std::function<void()>* late_callback = nullptr, bool invoke_callback = true,
                    bool* callback_threw = nullptr)
    -> nr::startup::ProbeResult<std::decay_t<std::invoke_result_t<Probe&>>> {
  using Result = std::decay_t<std::invoke_result_t<Probe&>>;
  std::mutex mutex;
  std::condition_variable queued_cv;
  bool queued = false;
  std::function<void()> stored;
  std::optional<nr::startup::ProbeResult<Result>> result;
  std::thread probe_thread([&] {
    result.emplace(nr::startup::ProbeOnWorker(
        std::move(probe),
        [&](std::function<void()> callback) {
          {
            std::lock_guard<std::mutex> lock(mutex);
            stored = std::move(callback);
            queued = true;
          }
          queued_cv.notify_one();
        },
        timeout));
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    queued_cv.wait(lock, [&] { return queued; });
  }
  std::function<void()> callback;
  {
    std::lock_guard<std::mutex> lock(mutex);
    callback = stored;
  }
  if (invoke_callback) {
    try {
      callback();
    } catch (...) {
      if (callback_threw) {
        *callback_threw = true;
      } else {
        throw;
      }
    }
  }
  probe_thread.join();
  if (late_callback) *late_callback = std::move(callback);
  return std::move(*result);
}

void TestProbeOnWorkerQueuedSuccess() {
  int calls = 0;
  const auto result = RunQueuedProbe([&] { ++calls; return 42; }, std::chrono::seconds(1));
  CHECK(result.status == nr::startup::ProbeStatus::kSucceeded);
  CHECK(result.value.has_value());
  CHECK(result.value && *result.value == 42);
  CHECK(!result.error);
  CHECK(calls == 1);
}

void TestProbeOnWorkerTimeout() {
  const auto result =
      RunQueuedProbe([] { return 1; }, std::chrono::milliseconds(10), nullptr, false);
  CHECK(result.status == nr::startup::ProbeStatus::kTimedOut);
  CHECK(!result.value);
  CHECK(!result.error);
}

void TestProbeOnWorkerLateCallbackIsHarmless() {
  std::function<void()> late_callback;
  bool probe_ran = false;
  const auto result = RunQueuedProbe([&] { probe_ran = true; return 7; },
                                     std::chrono::milliseconds(10), &late_callback, false);
  CHECK(result.status == nr::startup::ProbeStatus::kTimedOut);
  CHECK(static_cast<bool>(late_callback));
  bool escaped = false;
  try {
    late_callback();
  } catch (...) {
    escaped = true;
  }
  CHECK(!escaped);
  CHECK(probe_ran);
}

void TestProbeOnWorkerForwardsException() {
  bool callback_threw = false;
  const auto result = RunQueuedProbe(
      []() -> int { throw std::runtime_error("probe failed"); }, std::chrono::seconds(1), nullptr,
      true, &callback_threw);
  CHECK(!callback_threw);  // the command-processor callback must not throw
  CHECK(result.status == nr::startup::ProbeStatus::kThrew);
  CHECK(!result.value);
  CHECK(result.error != nullptr);
  bool rethrown = false;
  try {
    std::rethrow_exception(result.error);
  } catch (const std::runtime_error& error) {
    rethrown = std::string_view(error.what()) == "probe failed";
  }
  CHECK(rethrown);
}

}  // namespace

int main() {
  TestSelectorPermutations();
  TestBackendStartupGateSelection();
  TestGuidanceIsVerbatim();
  TestUiTextIsPlainLanguage();
  TestMissingFeatureDetails();
  TestDecisionPermutations();
  TestCopyPositionalArguments();
  TestInitializeForwardsArgumentsAndResult();
  TestRefusalBecomesOneNotification();
  TestUnrelatedExceptionPropagates();
  TestProbeOnWorkerQueuedSuccess();
  TestProbeOnWorkerTimeout();
  TestProbeOnWorkerLateCallbackIsHarmless();
  TestProbeOnWorkerForwardsException();

  std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
