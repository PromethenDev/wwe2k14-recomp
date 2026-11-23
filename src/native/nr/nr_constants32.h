// Shared policy for the opt-in 32-bit constants ABI (G56 B1).
//
// The shader set chooses how constants are addressed:
//   legacy      : SPIR-V reads the constants ring through 64-bit buffer device addresses.
//   constants32 : SPIR-V reads the ring as ByteAddressBuffer set 4 binding 0, with the three
//                 pushed values as 32-bit byte offsets (base 0). No Int64 in the shaders.
// The switch is default-off and must agree with a regular-file `constants32` marker in the
// primary shader folder. Standard library only: this header is also compiled by the CPU test.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace nr_constants32 {

inline constexpr const char* kSwitch = "WWE2K14_NR_CONSTANTS32";
inline constexpr const char* kMarkerName = "constants32";
inline constexpr const char* kPrimaryFolderSwitch = "WWE2K14_NR_SHADERS";
inline constexpr const char* kSecondaryFolderSwitch = "WWE2K14_NR_SHADERS_B";
inline constexpr const char* kDefaultShaders = "out/nr/shaders";

// Mirrors nr_backend.cpp: one ring slot is guard + vs + ps + shared + guard.
inline constexpr uint64_t kGuardBytes = 4096;
inline constexpr uint64_t kSlotBytes = kGuardBytes + 4096 + 4096 + 1024 + kGuardBytes;
inline constexpr uint64_t kSlots = 12288;

// Exact opt-in value. Anything else ("0", "true", "01", " 1", empty) is off.
inline bool SwitchRequested(const char* value) {
  return value != nullptr && value[0] == '1' && value[1] == '\0';
}

// Primary folder: exactly what d3d/runtime.cpp loads (getenv != null, empty included), otherwise
// the default folder. Matching runtime keeps the marker check on the folder whose shaders load.
inline std::string PrimaryFolder(const char* value) {
  return value != nullptr ? std::string(value) : std::string(kDefaultShaders);
}

// Secondary folder: empty when unset.
inline std::string SecondaryFolder(const char* value) {
  return value != nullptr ? std::string(value) : std::string();
}

struct MarkerStatus {
  bool present = false;  // a regular file named `constants32`
  bool error = false;    // stat failed, or the name exists but is not a regular file
  std::string error_text;
};

inline MarkerStatus InspectMarker(const std::string& folder) {
  MarkerStatus out;
  std::error_code ec;
  const std::filesystem::path marker = std::filesystem::path(folder) / kMarkerName;
  const std::filesystem::file_status st = std::filesystem::status(marker, ec);
  // A missing folder or marker is simply "absent"; only a real stat failure is an error.
  if (ec == std::errc::no_such_file_or_directory) return out;
  if (ec) {
    out.error = true;
    out.error_text = "cannot stat " + marker.string() + ": " + ec.message();
    return out;
  }
  if (st.type() == std::filesystem::file_type::not_found) return out;
  if (!std::filesystem::is_regular_file(st)) {
    out.error = true;
    out.error_text = (std::filesystem::path(folder) / kMarkerName).string() +
                     " exists but is not a regular file";
    return out;
  }
  out.present = true;
  return out;
}

struct Selection {
  bool ok = false;
  bool enabled = false;
  std::string error;
};

// Switch/marker agreement, plus no mixed ABI when a secondary folder is configured.
inline Selection Select(bool switch_requested, bool secondary_configured,
                        const MarkerStatus& primary, const MarkerStatus& secondary) {
  Selection out;
  if (primary.error) {
    out.error = primary.error_text;
    return out;
  }
  if (secondary_configured && secondary.error) {
    out.error = secondary.error_text;
    return out;
  }
  if (switch_requested != primary.present) {
    out.error = switch_requested
                    ? std::string(kSwitch) + "=1 but no regular " + kMarkerName + " marker in the primary shader folder"
                    : std::string(kMarkerName) + " marker present but " + kSwitch + " is not exactly \"1\"";
    return out;
  }
  if (secondary_configured && secondary.present != primary.present) {
    out.error = "mixed constants ABI: primary and secondary shader folders disagree on the " +
                std::string(kMarkerName) + " marker";
    return out;
  }
  out.ok = true;
  out.enabled = primary.present;
  return out;
}

// Reads the switch and both marker folders for the backend. `primary` is the folder the backend
// actually loads (the Init(shader_dir) argument), never an ambient env lookup here; `secondary`
// is the resolved WWE2K14_NR_SHADERS_B value, where empty means "not configured".
inline Selection ReadSelection(const std::string& primary, const std::string& secondary,
                               const char* flag) {
  return Select(SwitchRequested(flag), !secondary.empty(), InspectMarker(primary),
                secondary.empty() ? MarkerStatus{} : InspectMarker(secondary));
}

// The backend ABI selected for this run.
struct AbiPlan {
  bool constants32 = false;
  uint32_t descriptor_set_count = 4;        // sets 0..3 legacy, 0..4 constants32 (set 4 = constants)
  bool constants_ring_addressable = true;   // legacy: reads through BDA addresses
  bool constants_ring_storage = false;      // constants32: STORAGE_BUFFER, whole ring at set 4 binding 0
  bool bind_constants_set_every_draw = false;
  bool frame_slots_eligible = true;
  bool rt1_eligible = true;
  bool requires_buffer_device_address = true;      // both ABIs: backend capability check stays
  bool requires_descriptor_indexing = true;        // both ABIs: texture/sampler tables stay
  bool constants_use_int64 = true;                 // constants32: false (32-bit byte offsets)
};

inline AbiPlan Plan(bool enabled) {
  AbiPlan p;
  p.constants32 = enabled;
  p.descriptor_set_count = enabled ? 5u : 4u;
  p.constants_ring_addressable = !enabled;
  p.constants_ring_storage = enabled;
  p.bind_constants_set_every_draw = enabled;
  p.frame_slots_eligible = !enabled;  // FRAME_SLOTS is disabled for constants32 (single ring)
  p.rt1_eligible = !enabled;          // RT1 keeps the Int64 ABI
  p.constants_use_int64 = !enabled;
  return p;
}

// Base added to a ring slot offset before pushing. Legacy Vulkan pushes the buffer device
// address; constants32 and D3D12 push byte offsets into the ring (base 0).
inline uint64_t ConstantAddressBase(const AbiPlan& plan, bool spirv, uint64_t device_address) {
  if (plan.constants32 || !spirv) return 0;
  return device_address;
}

inline uint32_t PushLow(uint64_t value) { return static_cast<uint32_t>(value); }
inline uint32_t PushHigh(uint64_t value) { return static_cast<uint32_t>(value >> 32); }

// constants32 pushes must fit in 32 bits: high dword zero.
inline bool Fits32(uint64_t value) { return (value >> 32) == 0; }

inline bool RingInBounds(uint64_t offset, uint64_t bytes, uint64_t ring_bytes) {
  return offset <= ring_bytes && bytes <= ring_bytes - offset;
}

}  // namespace nr_constants32
