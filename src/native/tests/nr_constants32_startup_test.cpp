// CPU test for nr_constants32_startup.h: the lazy startup choice never performs real IO or
// touches the environment; the fakes below stand in for both probes.
#include <cstdio>
#include <string>

#include "native/nr/nr_constants32_startup.h"

namespace {

using nr_constants32::ChoiceInput;
using nr_constants32::ChoiceResult;
using nr_constants32::DecideStartupChoice;
using nr_constants32::MarkerStatus;
using nr_constants32::Selection;
using nr_constants32::Sibling32Folder;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
  if (condition) return;
  std::printf("FAIL: %s\n", what.c_str());
  ++g_failures;
}

// Fake inspectors: count calls and answer from fixed per-folder statuses.
struct FakeProbe {
  int marker_calls = 0;
  int complete_calls = 0;
  std::string last_marker_folder;
  std::string last_legacy;
  std::string last_candidate;
  std::string secondary_folder;
  MarkerStatus primary_status;
  MarkerStatus secondary_status;
  Selection complete_result;

  MarkerStatus operator()(const std::string& folder) {
    ++marker_calls;
    last_marker_folder = folder;
    if (!secondary_folder.empty() && folder == secondary_folder) return secondary_status;
    return primary_status;
  }

  Selection operator()(const std::string& legacy, const std::string& candidate) {
    ++complete_calls;
    last_legacy = legacy;
    last_candidate = candidate;
    return complete_result;
  }
};

ChoiceInput BaseInput() {
  ChoiceInput in;
  in.native_selected = true;
  in.direct_present = true;
  in.output_populated = true;
  in.shader_int64 = false;
  in.buffer_device_address = true;
  in.descriptor_indexing = true;
  in.manual_force = false;
  in.primary = "out/nr/shaders";
  return in;  // secondary empty
}

ChoiceInput FromMask(unsigned mask) {
  ChoiceInput in = BaseInput();
  in.native_selected = (mask & (1u << 0)) != 0;
  in.direct_present = (mask & (1u << 1)) != 0;
  in.output_populated = (mask & (1u << 2)) != 0;
  in.shader_int64 = (mask & (1u << 3)) != 0;
  in.buffer_device_address = (mask & (1u << 4)) != 0;
  in.descriptor_indexing = (mask & (1u << 5)) != 0;
  in.manual_force = (mask & (1u << 6)) != 0;
  in.secondary = "out/nr/shaders_b";
  return in;
}

bool CapabilitiesPresent(const ChoiceInput& in) {
  return in.native_selected && in.direct_present && in.output_populated &&
         in.buffer_device_address && in.descriptor_indexing;
}

std::string Tag(unsigned mask, const char* what) {
  return "mask " + std::to_string(mask) + " (" + what + ")";
}

void TestSibling32Folder() {
  Check(Sibling32Folder("out/nr/shaders") == "out/nr/shaders32", "sibling: plain");
  Check(Sibling32Folder("out/nr/shaders/") == "out/nr/shaders32", "sibling: one slash");
  Check(Sibling32Folder("out/nr/shaders///") == "out/nr/shaders32", "sibling: many slashes");
  Check(Sibling32Folder("out/nr/shaders/.") == "out/nr/shaders32", "sibling: trailing dot");
  Check(Sibling32Folder("out/nr/shaders/./") == "out/nr/shaders32", "sibling: trailing dot slash");
  Check(Sibling32Folder("/a/b//") == "/a/shaders32", "sibling: arbitrary primary name");
  Check(Sibling32Folder("out/nr/shaders-s375") == "out/nr/shaders32", "sibling: development set");
  Check(Sibling32Folder("/") == "/shaders32", "sibling: root");
  Check(Sibling32Folder("") == "shaders32", "sibling: empty primary");
}

void TestAllBooleanCombinations() {
  for (unsigned mask = 0; mask < 128u; ++mask) {
    const ChoiceInput in = FromMask(mask);
    FakeProbe probe;
    probe.secondary_folder = in.secondary;
    probe.primary_status.present = true;
    probe.secondary_status.present = true;
    probe.complete_result.ok = true;
    probe.complete_result.enabled = true;

    const ChoiceResult r = DecideStartupChoice(in, probe, probe);
    const bool eligible = CapabilitiesPresent(in) && (!in.shader_int64 || in.manual_force);

    Check(r.error.empty(), Tag(mask, "no error"));
    if (!eligible) {
      Check(probe.marker_calls == 0, Tag(mask, "no marker callbacks"));
      Check(probe.complete_calls == 0, Tag(mask, "no complete callbacks"));
      Check(!r.activate && !r.reprobe, Tag(mask, "inert"));
      Check(r.primary == in.primary, Tag(mask, "legacy path kept"));
      continue;
    }
    Check(r.activate, Tag(mask, "activated"));
    Check(r.reprobe, Tag(mask, "reprobe requested"));
    if (in.manual_force) {
      // Marked primary: primary marker + configured secondary marker, no completeness probe.
      Check(probe.marker_calls == 2, Tag(mask, "manual marker calls"));
      Check(probe.complete_calls == 0, Tag(mask, "manual complete calls"));
      Check(probe.last_marker_folder == in.secondary, Tag(mask, "manual secondary inspected"));
      Check(r.primary == in.primary, Tag(mask, "manual primary preserved"));
    } else {
      Check(probe.marker_calls == 1, Tag(mask, "auto marker calls"));
      Check(probe.complete_calls == 1, Tag(mask, "auto complete calls"));
      Check(probe.last_legacy == in.primary, Tag(mask, "auto legacy folder"));
      Check(probe.last_candidate == "out/nr/shaders32", Tag(mask, "auto candidate folder"));
      Check(r.primary == "out/nr/shaders32", Tag(mask, "auto primary replaced"));
    }
  }
}

void TestManualInt64MarkedPrimary() {
  ChoiceInput in = BaseInput();
  in.manual_force = true;
  in.shader_int64 = true;  // manual bypasses only the Int64 requirement
  FakeProbe probe;
  probe.primary_status.present = true;

  const ChoiceResult r = DecideStartupChoice(in, probe, probe);
  Check(r.error.empty(), "manual int64: no error");
  Check(probe.marker_calls == 1, "manual int64: one marker probe");
  Check(probe.complete_calls == 0, "manual int64: no completeness probe");
  Check(r.primary == in.primary, "manual int64: primary preserved");
  Check(r.activate, "manual int64: activated");
  Check(r.reprobe, "manual int64: reprobe");
}

void TestManualInt64Sibling() {
  ChoiceInput in = BaseInput();
  in.manual_force = true;
  in.shader_int64 = true;
  FakeProbe probe;  // legacy primary has no marker
  probe.complete_result = {true, true, {}};
  const ChoiceResult r = DecideStartupChoice(in, probe, probe);
  Check(probe.marker_calls == 1 && probe.complete_calls == 1, "manual legacy: inspect sibling");
  Check(r.primary == "out/nr/shaders32", "manual legacy: sibling selected on Int64 GPU");
  Check(r.activate && r.reprobe && r.error.empty(), "manual legacy: activate and reprobe");
  probe.complete_result = {true, false, {}};
  const ChoiceResult incomplete = DecideStartupChoice(in, probe, probe);
  Check(!incomplete.activate && !incomplete.reprobe && incomplete.primary == in.primary,
        "inspector must affirm the constants32 ABI");
}

void TestManualNeverBypassesCapabilities() {
  ChoiceInput in = BaseInput();
  in.manual_force = true;
  in.shader_int64 = true;
  in.buffer_device_address = false;
  FakeProbe probe;
  probe.primary_status.present = true;
  const ChoiceResult r = DecideStartupChoice(in, probe, probe);
  Check(probe.marker_calls == 0 && probe.complete_calls == 0, "manual cannot bypass BDA");
  Check(!r.activate && !r.reprobe && r.error.empty(), "manual cannot bypass BDA: inert");
  Check(r.primary == in.primary, "manual cannot bypass BDA: primary preserved");

  in.buffer_device_address = true;
  in.descriptor_indexing = false;
  FakeProbe probe2;
  probe2.primary_status.present = true;
  const ChoiceResult r2 = DecideStartupChoice(in, probe2, probe2);
  Check(probe2.marker_calls == 0 && probe2.complete_calls == 0,
        "manual cannot bypass descriptor indexing");
  Check(!r2.activate && !r2.reprobe, "manual cannot bypass DI: inert");
}

void TestExplicitMarkedPrimary() {
  ChoiceInput in = BaseInput();
  in.manual_force = true;
  in.secondary = "out/nr/shaders_b";
  FakeProbe probe;
  probe.secondary_folder = in.secondary;
  probe.primary_status.present = true;
  probe.secondary_status.present = true;

  const ChoiceResult r = DecideStartupChoice(in, probe, probe);
  Check(r.error.empty(), "marked primary: no error");
  Check(probe.marker_calls == 2, "marked primary: both markers probed");
  Check(probe.complete_calls == 0, "marked primary: no completeness probe");
  Check(r.primary == in.primary, "marked primary: path preserved");
  Check(r.activate && r.reprobe, "marked primary: activated");
}

void TestIncompleteSibling() {
  ChoiceInput in = BaseInput();
  in.secondary = "out/nr/shaders_b";
  FakeProbe probe;
  probe.secondary_folder = in.secondary;
  probe.complete_result.ok = false;

  const ChoiceResult r = DecideStartupChoice(in, probe, probe);
  Check(r.error.empty(), "incomplete sibling: no error");
  Check(!r.activate && !r.reprobe, "incomplete sibling: inert");
  Check(r.primary == in.primary, "incomplete sibling: legacy path kept");
  Check(probe.marker_calls == 0, "incomplete sibling: no marker probe");
  Check(probe.complete_calls == 1, "incomplete sibling: one completeness probe");
  Check(probe.last_legacy == in.primary, "incomplete sibling: legacy folder passed");
  Check(probe.last_candidate == "out/nr/shaders32", "incomplete sibling: candidate passed");

  FakeProbe probe2;
  probe2.complete_result.ok = false;
  probe2.complete_result.error = "probe failed";
  const ChoiceResult r2 = DecideStartupChoice(in, probe2, probe2);
  Check(r2.error == "probe failed", "errored sibling: probe error reported");
  Check(!r2.activate && r2.primary == in.primary, "errored sibling: legacy path kept");
}

void TestMixedSecondary() {
  ChoiceInput in = BaseInput();  // auto
  in.secondary = "out/nr/shaders_b";
  FakeProbe probe;
  probe.secondary_folder = in.secondary;
  probe.secondary_status.present = false;  // secondary lacks the marker
  probe.complete_result.ok = true;
  probe.complete_result.enabled = true;

  const ChoiceResult r = DecideStartupChoice(in, probe, probe);
  Check(!r.error.empty(), "mixed secondary: error reported");
  Check(r.error.find("mixed") != std::string::npos, "mixed secondary: error names the mix");
  Check(!r.activate && !r.reprobe, "mixed secondary: inert");
  Check(r.primary == in.primary, "mixed secondary: legacy path kept");
  Check(probe.marker_calls == 1, "mixed secondary: secondary marker probed once");
  Check(probe.complete_calls == 1, "mixed secondary: one completeness probe");
}

void TestMarkerStatError() {
  ChoiceInput in = BaseInput();
  in.manual_force = true;
  FakeProbe probe;
  probe.primary_status.error = true;
  probe.primary_status.error_text = "stat boom";

  const ChoiceResult r = DecideStartupChoice(in, probe, probe);
  Check(r.error == "stat boom", "primary stat error: reported");
  Check(probe.marker_calls == 1 && probe.complete_calls == 0, "primary stat error: stops probes");
  Check(!r.activate && !r.reprobe && r.primary == in.primary, "primary stat error: inert");

  in.secondary = "out/nr/shaders_b";
  FakeProbe probe2;
  probe2.secondary_folder = in.secondary;
  probe2.primary_status.present = true;
  probe2.secondary_status.error = true;
  probe2.secondary_status.error_text = "secondary boom";
  const ChoiceResult r2 = DecideStartupChoice(in, probe2, probe2);
  Check(r2.error == "secondary boom", "secondary stat error: reported");
  Check(probe2.marker_calls == 2 && probe2.complete_calls == 0,
        "secondary stat error: no completeness probe");
  Check(!r2.activate && r2.primary == in.primary, "secondary stat error: inert");
}

void TestTrailingSlash() {
  ChoiceInput in = BaseInput();  // auto
  in.primary = "out/nr/shaders///";
  FakeProbe probe;
  probe.complete_result.ok = true;
  probe.complete_result.enabled = true;

  const ChoiceResult r = DecideStartupChoice(in, probe, probe);
  Check(r.error.empty(), "trailing slash: no error");
  Check(probe.last_legacy == in.primary, "trailing slash: original folder passed as legacy");
  Check(probe.last_candidate == "out/nr/shaders32", "trailing slash: stripped candidate");
  Check(r.primary == "out/nr/shaders32", "trailing slash: sibling selected");
  Check(r.activate && r.reprobe, "trailing slash: activated");
}

void TestAutoSuccess() {
  ChoiceInput in = BaseInput();  // auto, no secondary
  FakeProbe probe;
  probe.complete_result.ok = true;
  probe.complete_result.enabled = true;

  const ChoiceResult r = DecideStartupChoice(in, probe, probe);
  Check(r.error.empty(), "auto success: no error");
  Check(probe.marker_calls == 0, "auto success: no marker probe without secondary");
  Check(probe.complete_calls == 1, "auto success: one completeness probe");
  Check(r.primary == "out/nr/shaders32", "auto success: sibling selected");
  Check(r.activate && r.reprobe, "auto success: activated");
}

}  // namespace

int main() {
  TestSibling32Folder();
  TestAllBooleanCombinations();
  TestManualInt64MarkedPrimary();
  TestManualInt64Sibling();
  TestManualNeverBypassesCapabilities();
  TestExplicitMarkedPrimary();
  TestIncompleteSibling();
  TestMixedSecondary();
  TestMarkerStatError();
  TestTrailingSlash();
  TestAutoSuccess();

  if (g_failures != 0) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("all nr_constants32 startup-choice tests passed\n");
  return 0;
}
