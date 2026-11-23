// Lazy startup choice for the opt-in 32-bit constants shader ABI (G56b).
//
// Pure policy: this header performs no filesystem IO and does not touch the environment; both
// disk-facing steps are injected by the caller as
//   inspect_marker(folder)              -> MarkerStatus
//   inspect_complete(legacy, candidate) -> Selection
// The marker rules themselves come from nr_constants32.h (Select) and are not reimplemented here.
#pragma once

#include <string>

#include "native/nr/nr_constants32.h"

namespace nr_constants32 {

struct ChoiceInput {
  // Backend facts for this run.
  bool native_selected = false;
  bool direct_present = false;
  bool output_populated = false;
  bool shader_int64 = false;  // enabled feature reported by the selected SDK device
  bool buffer_device_address = false;
  bool descriptor_indexing = false;
  bool manual_force = false;  // operator override of the Int64 requirement only
  std::string primary;
  std::string secondary;
};

struct ChoiceResult {
  std::string primary;  // starts as input.primary; only a chosen sibling replaces it
  bool activate = false;
  bool reprobe = false;
  std::string error;
};

// One canonical install name, beside the selected primary (which need not be named shaders).
inline std::string Sibling32Folder(const std::string& primary) {
  std::filesystem::path folder(primary);
  while (folder.has_relative_path() && (folder.filename().empty() || folder.filename() == ".")) {
    folder = folder.parent_path();
  }
  return (folder.parent_path() / "shaders32").string();
}

// Chooses once, lazily:
//   * missing backend capabilities, or Int64 shaders without manual_force, return with no IO;
//   * manual runs honour an explicitly marked primary folder unchanged;
//   * otherwise the only opt-in is a complete sibling constants32 folder that agrees with the
//     secondary folder's marker.
template <typename InspectMarker, typename InspectComplete>
ChoiceResult DecideStartupChoice(const ChoiceInput& input, InspectMarker&& inspect_marker,
                                 InspectComplete&& inspect_complete) {
  ChoiceResult out;
  out.primary = input.primary;

  // constants32 needs the same backend capabilities as the legacy ABI; manual_force never
  // bypasses these.
  if (!input.native_selected || !input.direct_present || !input.output_populated ||
      !input.buffer_device_address || !input.descriptor_indexing) {
    return out;
  }
  // Keep the existing shader ABI on capable devices unless the operator forces a test.
  if (input.shader_int64 && !input.manual_force) return out;

  const bool secondary_configured = !input.secondary.empty();

  if (input.manual_force) {
    const MarkerStatus primary_marker = inspect_marker(input.primary);
    if (primary_marker.error) {
      out.error = primary_marker.error_text;
      if (out.error.empty()) {
        out.error = std::string("cannot inspect ") + kMarkerName + " marker in " + input.primary;
      }
      return out;
    }
    if (primary_marker.present) {
      // An explicitly marked primary stays authoritative for manual runs; only ABI agreement
      // with a configured secondary folder can still refuse it.
      const MarkerStatus secondary_marker =
          secondary_configured ? inspect_marker(input.secondary) : MarkerStatus{};
      const Selection sel = Select(true, secondary_configured, primary_marker, secondary_marker);
      if (!sel.ok) {
        out.error = sel.error;
        return out;
      }
      out.activate = sel.enabled;
      out.reprobe = sel.enabled;
      return out;
    }
  }

  // No marked primary to honour: the remaining opt-in is a complete sibling shader folder.
  const std::string candidate = Sibling32Folder(input.primary);
  const Selection complete = inspect_complete(input.primary, candidate);
  if (!complete.ok || !complete.enabled) {
    // Absent/incomplete sibling: keep the original (legacy) folder. A probe error is reported
    // but never fatal here; the run continues on the primary folder.
    if (!complete.error.empty()) out.error = complete.error;
    return out;
  }

  MarkerStatus marked_candidate;
  marked_candidate.present = true;  // completeness implies the marker is there
  const MarkerStatus secondary_marker =
      secondary_configured ? inspect_marker(input.secondary) : MarkerStatus{};
  const Selection sel = Select(true, secondary_configured, marked_candidate, secondary_marker);
  if (!sel.ok) {
    out.error = sel.error;
    return out;
  }
  if (sel.enabled) {
    out.primary = candidate;
    out.activate = true;
    out.reprobe = true;
  }
  return out;
}

}  // namespace nr_constants32
