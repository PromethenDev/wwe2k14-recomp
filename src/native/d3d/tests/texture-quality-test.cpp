// Host-only tests for the pure sampling-quality core (Q1b).
//
// No engine, D3D, GPU, or game dependency. SPIR-V modules are assembled in
// memory; optional CLI arguments name retained .spv files to analyse as
// fragment shaders and print their proof outcome.

#include "sampling_quality_core.h"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <vector>

namespace q = wwe2k14::d3d::quality;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* expr, const char* file, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
  }
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

using Words = std::vector<uint32_t>;

Words NewModule(uint32_t bound = 32u, uint32_t version = 0x00010000u) {
  return Words{0x07230203u, version, 0u, bound, 0u};
}

void Emit(Words& words, uint32_t opcode,
          std::initializer_list<uint32_t> operands) {
  // SPIR-V's word count includes the opcode word itself.
  words.push_back((static_cast<uint32_t>(operands.size() + 1u) << 16u) | opcode);
  words.insert(words.end(), operands.begin(), operands.end());
}

// Entry point named "main" with id |id| and the given execution model.
void EmitEntry(Words& words, uint32_t stage, uint32_t id) {
  Emit(words, 15u, {stage, id, 0x6E69616Du, 0u});
}

void EmitFunctionStart(Words& words, uint32_t id) {
  Emit(words, 54u, {1u, id, 0u, 2u});
  Emit(words, 248u, {6u});
}

void EmitFunctionEnd(Words& words) {
  Emit(words, 253u, {});
  Emit(words, 56u, {});
}

q::SamplingProof Prove(const Words& words, bool vertex) {
  return q::ProveSpirvSampling(
      reinterpret_cast<const uint8_t*>(words.data()),
      words.size() * sizeof(uint32_t), vertex);
}

Words VertexModule() {
  Words words = NewModule();
  EmitEntry(words, 0u, 5u);
  EmitFunctionStart(words, 5u);
  EmitFunctionEnd(words);
  return words;
}

Words FragmentModuleNoSample() {
  Words words = NewModule();
  EmitEntry(words, 4u, 5u);
  EmitFunctionStart(words, 5u);
  EmitFunctionEnd(words);
  return words;
}

Words FragmentModuleWith(uint32_t opcode,
                         std::initializer_list<uint32_t> operands) {
  Words words = NewModule();
  EmitEntry(words, 4u, 5u);
  EmitFunctionStart(words, 5u);
  Emit(words, opcode, operands);
  EmitFunctionEnd(words);
  return words;
}

uint32_t Bits(float value) { return std::bit_cast<uint32_t>(value); }

q::SamplingQualityInfo EligibleInfo() {
  q::SamplingQualityInfo info;
  info.known = true;
  info.ordinary_guest = true;
  info.sampled_only = true;
  info.render_target = false;
  info.resolve_target = false;
  info.cpu_updated = false;
  info.late_upload = false;
  info.two_d = true;
  info.width = 1024u;
  info.height = 1024u;
  info.base_level = 0u;
  info.resident_levels = 5u;
  info.sampling_proof_safe = true;
  return info;
}

void TestParseMode() {
  CHECK(q::ParseMode(nullptr) == 0u);
  CHECK(q::ParseMode("") == 0u);
  CHECK(q::ParseMode("1") == 1u);
  CHECK(q::ParseMode("2") == 2u);
  CHECK(q::ParseMode("0") == 0u);
  CHECK(q::ParseMode("3") == 0u);
  CHECK(q::ParseMode("01") == 0u);
  CHECK(q::ParseMode("10") == 0u);
  CHECK(q::ParseMode("1x") == 0u);
  CHECK(q::ParseMode(" 1") == 0u);
}

void TestProofHeaderAndFraming() {
  const Words vertex = VertexModule();
  const auto ok = Prove(vertex, true);
  CHECK(ok.parsed);
  CHECK(ok.safe);
  CHECK(ok.samples == 0u);
  CHECK(ok.reasons == 0u);

  const auto no_sample_ps = Prove(FragmentModuleNoSample(), false);
  CHECK(no_sample_ps.parsed);
  CHECK(!no_sample_ps.safe);
  CHECK(no_sample_ps.samples == 0u);
  CHECK(no_sample_ps.reasons == q::kProofNoSamples);

  const auto null_bytes = q::ProveSpirvSampling(nullptr, 20u, true);
  CHECK(!null_bytes.parsed);
  CHECK(!null_bytes.safe);
  CHECK((null_bytes.reasons & q::kProofMalformed) != 0u);

  uint8_t tiny[20]{};
  const auto short_bytes = q::ProveSpirvSampling(tiny, 19u, true);
  CHECK(!short_bytes.parsed);
  CHECK((short_bytes.reasons & q::kProofMalformed) != 0u);

  const auto unaligned = q::ProveSpirvSampling(
      reinterpret_cast<const uint8_t*>(vertex.data()),
      vertex.size() * sizeof(uint32_t) - 1u, true);
  CHECK(!unaligned.parsed);
  CHECK((unaligned.reasons & q::kProofMalformed) != 0u);

  const auto expect_unsupported = [](Words words) {
    const auto proof = Prove(words, true);
    CHECK(!proof.parsed);
    CHECK(!proof.safe);
    CHECK((proof.reasons & q::kProofUnsupportedHeader) != 0u);
  };

  Words w = vertex;
  w[0] = 0x07230204u;
  expect_unsupported(w);

  w = vertex;
  w[1] = 0x00020000u;  // SPIR-V 2.0
  expect_unsupported(w);

  w = vertex;
  w[1] = 0x00010007u;  // SPIR-V 1.7
  expect_unsupported(w);

  w = vertex;
  w[1] = 0x00010001u;  // low version bits must be zero
  expect_unsupported(w);

  w = vertex;
  w[2] = 0x000E0000u;  // generator word is tool identity, not validated
  CHECK(Prove(w, true).parsed);

  w = vertex;
  w[3] = 0u;  // bound
  expect_unsupported(w);

  w = vertex;
  w[3] = (1u << 22u) + 1u;
  expect_unsupported(w);

  w = vertex;
  w[4] = 1u;  // reserved
  expect_unsupported(w);

  Words v16 = vertex;
  v16[1] = 0x00010600u;  // SPIR-V 1.6 is accepted
  const auto p16 = Prove(v16, true);
  CHECK(p16.parsed);
  CHECK(p16.safe);

  // Header only.
  auto p = Prove(NewModule(), true);
  CHECK(!p.parsed);
  CHECK((p.reasons & q::kProofMalformed) != 0u);

  // Zero-word instruction.
  w = NewModule();
  w.push_back(0u);
  p = Prove(w, true);
  CHECK(!p.parsed);
  CHECK((p.reasons & q::kProofMalformed) != 0u);

  // Instruction claims more words than remain.
  w = NewModule();
  w.push_back((5u << 16u) | 15u);
  p = Prove(w, true);
  CHECK(!p.parsed);
  CHECK((p.reasons & q::kProofMalformed) != 0u);

  // Entry point but no function.
  w = NewModule();
  EmitEntry(w, 0u, 5u);
  p = Prove(w, true);
  CHECK(!p.parsed);
  CHECK((p.reasons & q::kProofMalformed) != 0u);

  // Function end without function start.
  w = NewModule();
  EmitEntry(w, 0u, 5u);
  Emit(w, 56u, {});
  p = Prove(w, true);
  CHECK(!p.parsed);
  CHECK((p.reasons & q::kProofMalformed) != 0u);

  // Unterminated function.
  w = NewModule();
  EmitEntry(w, 0u, 5u);
  EmitFunctionStart(w, 5u);
  Emit(w, 253u, {});
  p = Prove(w, true);
  CHECK(!p.parsed);
  CHECK((p.reasons & q::kProofMalformed) != 0u);

  // Malformed OpFunction words.
  w = NewModule();
  EmitEntry(w, 0u, 5u);
  Emit(w, 54u, {1u, 5u, 0u});
  p = Prove(w, true);
  CHECK(!p.parsed);
  CHECK((p.reasons & q::kProofMalformed) != 0u);

  // OpFunction id zero and out of bound.
  w = NewModule();
  EmitEntry(w, 0u, 5u);
  Emit(w, 54u, {1u, 0u, 0u, 2u});
  p = Prove(w, true);
  CHECK(!p.parsed);
  CHECK((p.reasons & q::kProofMalformed) != 0u);

  w = NewModule();
  EmitEntry(w, 0u, 5u);
  Emit(w, 54u, {1u, 32u, 0u, 2u});
  p = Prove(w, true);
  CHECK(!p.parsed);
  CHECK((p.reasons & q::kProofMalformed) != 0u);

  // Malformed OpFunctionEnd words.
  w = NewModule();
  EmitEntry(w, 0u, 5u);
  EmitFunctionStart(w, 5u);
  Emit(w, 253u, {});
  Emit(w, 56u, {0u});
  p = Prove(w, true);
  CHECK(!p.parsed);
  CHECK((p.reasons & q::kProofMalformed) != 0u);

  // Entry point with too few words.
  w = NewModule();
  Emit(w, 15u, {0u, 5u});
  p = Prove(w, true);
  CHECK(!p.parsed);
  CHECK((p.reasons & q::kProofMalformed) != 0u);

  // Entry point after a function has started.
  w = NewModule();
  EmitFunctionStart(w, 5u);
  EmitEntry(w, 0u, 5u);
  p = Prove(w, true);
  CHECK(!p.parsed);
  CHECK((p.reasons & q::kProofMalformed) != 0u);

  // Entry point id zero and out of bound.
  w = NewModule();
  EmitEntry(w, 0u, 0u);
  p = Prove(w, true);
  CHECK(!p.parsed);
  CHECK((p.reasons & q::kProofMalformed) != 0u);

  w = NewModule();
  EmitEntry(w, 0u, 32u);
  p = Prove(w, true);
  CHECK(!p.parsed);
  CHECK((p.reasons & q::kProofMalformed) != 0u);

  // The entry id has no matching OpFunction.
  w = NewModule();
  EmitEntry(w, 0u, 9u);
  EmitFunctionStart(w, 5u);
  EmitFunctionEnd(w);
  p = Prove(w, true);
  CHECK(!p.parsed);
  CHECK((p.reasons & q::kProofMalformed) != 0u);

  // Two entry points, first one defined: parsed, not safe.
  w = NewModule();
  EmitEntry(w, 0u, 5u);
  EmitEntry(w, 4u, 7u);
  EmitFunctionStart(w, 5u);
  EmitFunctionEnd(w);
  p = Prove(w, true);
  CHECK(p.parsed);
  CHECK(!p.safe);
  CHECK((p.reasons & q::kProofEntryPoint) != 0u);

  // Unknown/reserved opcode inside a valid function.
  w = NewModule();
  EmitEntry(w, 0u, 5u);
  EmitFunctionStart(w, 5u);
  Emit(w, 216u, {});
  EmitFunctionEnd(w);
  p = Prove(w, true);
  CHECK(p.parsed);
  CHECK(!p.safe);
  CHECK((p.reasons & q::kProofUnknownOpcode) != 0u);

  w = NewModule();
  EmitEntry(w, 0u, 5u);
  EmitFunctionStart(w, 5u);
  Emit(w, 4000u, {});
  EmitFunctionEnd(w);
  p = Prove(w, true);
  CHECK(p.parsed);
  CHECK(!p.safe);
  CHECK((p.reasons & q::kProofUnknownOpcode) != 0u);

  // Stage validation.
  auto wrong_stage = Prove(VertexModule(), false);
  CHECK(wrong_stage.parsed);
  CHECK(!wrong_stage.safe);
  CHECK((wrong_stage.reasons & q::kProofWrongStage) != 0u);
  CHECK((wrong_stage.reasons & q::kProofNoSamples) != 0u);

  wrong_stage = Prove(FragmentModuleWith(87u, {1u, 7u, 8u, 9u}), true);
  CHECK(wrong_stage.parsed);
  CHECK(!wrong_stage.safe);
  CHECK((wrong_stage.reasons & q::kProofWrongStage) != 0u);
}

void TestProofImageForms() {
  // Ordinary implicit sampling, no optional operands.
  auto p = Prove(FragmentModuleWith(87u, {1u, 7u, 8u, 9u}), false);
  CHECK(p.parsed);
  CHECK(p.safe);
  CHECK(p.samples == 1u);
  CHECK(p.reasons == 0u);

  // Optional Bias/Offset/Sample/MinLod forms are not explicit Lod.
  p = Prove(FragmentModuleWith(87u, {1u, 7u, 8u, 9u, 1u, 10u}), false);
  CHECK(p.safe);
  CHECK(p.samples == 1u);

  // Implicit sampling with an explicit Lod operand is rejected.
  p = Prove(FragmentModuleWith(87u, {1u, 7u, 8u, 9u, 2u, 10u}), false);
  CHECK(p.parsed);
  CHECK(!p.safe);
  CHECK(p.samples == 1u);
  CHECK((p.reasons & q::kProofExplicitLod) != 0u);

  // Grad is not an implicit-Lod operand.
  p = Prove(FragmentModuleWith(87u, {1u, 7u, 8u, 9u, 4u, 10u, 11u}), false);
  CHECK(p.parsed);
  CHECK(!p.safe);
  CHECK((p.reasons & q::kProofImageOperands) != 0u);

  // Empty image-operand mask.
  p = Prove(FragmentModuleWith(87u, {1u, 7u, 8u, 9u, 0u}), false);
  CHECK(!p.safe);
  CHECK((p.reasons & q::kProofImageOperands) != 0u);

  // Unknown image-operand bit.
  p = Prove(FragmentModuleWith(87u, {1u, 7u, 8u, 9u, 0x100u}), false);
  CHECK(!p.safe);
  CHECK((p.reasons & q::kProofImageOperands) != 0u);

  // Missing coordinate.
  p = Prove(FragmentModuleWith(87u, {1u, 7u, 8u}), false);
  CHECK(p.parsed);
  CHECK(!p.safe);
  CHECK((p.reasons & q::kProofBadImageForm) != 0u);

  // Exact Grad form for explicit Lod is allowed.
  p = Prove(FragmentModuleWith(88u, {1u, 7u, 8u, 9u, 4u, 10u, 11u}), false);
  CHECK(p.parsed);
  CHECK(p.safe);
  CHECK(p.samples == 1u);
  CHECK(p.reasons == 0u);

  // Explicit Lod operand is rejected.
  p = Prove(FragmentModuleWith(88u, {1u, 7u, 8u, 9u, 2u, 10u}), false);
  CHECK(p.parsed);
  CHECK(!p.safe);
  CHECK(p.samples == 1u);
  CHECK((p.reasons & q::kProofExplicitLod) != 0u);
  CHECK((p.reasons & q::kProofBadExplicitForm) != 0u);

  // Grad with a missing operand.
  p = Prove(FragmentModuleWith(88u, {1u, 7u, 8u, 9u, 4u, 10u}), false);
  CHECK(p.parsed);
  CHECK(!p.safe);
  CHECK((p.reasons & q::kProofBadExplicitForm) != 0u);

  // Explicit form with no mask word.
  p = Prove(FragmentModuleWith(88u, {1u, 7u, 8u, 9u}), false);
  CHECK(p.parsed);
  CHECK(!p.safe);
  CHECK((p.reasons & q::kProofBadExplicitForm) != 0u);

  // Grad plus an extra trailing word.
  p = Prove(FragmentModuleWith(88u, {1u, 7u, 8u, 9u, 4u, 10u, 11u, 12u}),
            false);
  CHECK(!p.safe);
  CHECK((p.reasons & q::kProofBadExplicitForm) != 0u);

  // Forbidden image operations 89..99.
  for (uint32_t opcode = 89u; opcode <= 99u; ++opcode) {
    p = Prove(FragmentModuleWith(opcode, {}), false);
    CHECK(p.parsed);
    CHECK(!p.safe);
    CHECK((p.reasons & q::kProofForbiddenImageOp) != 0u);
  }

  // Sparse image operations 305..320.
  for (uint32_t opcode = 305u; opcode <= 320u; ++opcode) {
    p = Prove(FragmentModuleWith(opcode, {}), false);
    CHECK(p.parsed);
    CHECK(!p.safe);
    CHECK((p.reasons & q::kProofSparseImageOp) != 0u);
  }

  // Well-shaped image/query forms composed with a real sample.
  Words w = NewModule();
  EmitEntry(w, 4u, 5u);
  EmitFunctionStart(w, 5u);
  Emit(w, 87u, {1u, 7u, 8u, 9u});
  Emit(w, 100u, {10u, 11u, 8u});
  Emit(w, 101u, {12u, 13u, 8u});
  Emit(w, 102u, {14u, 15u, 8u});
  Emit(w, 103u, {16u, 17u, 8u, 0u});
  Emit(w, 104u, {18u, 19u, 8u});
  Emit(w, 105u, {20u, 21u, 8u, 22u});
  Emit(w, 106u, {23u, 24u, 8u});
  Emit(w, 107u, {25u, 26u, 8u});
  EmitFunctionEnd(w);
  p = Prove(w, false);
  CHECK(p.parsed);
  CHECK(p.safe);
  CHECK(p.samples == 1u);
  CHECK(p.reasons == 0u);

  // Two samples are counted.
  w = NewModule();
  EmitEntry(w, 4u, 5u);
  EmitFunctionStart(w, 5u);
  Emit(w, 87u, {1u, 7u, 8u, 9u});
  Emit(w, 87u, {1u, 10u, 11u, 12u});
  EmitFunctionEnd(w);
  p = Prove(w, false);
  CHECK(p.safe);
  CHECK(p.samples == 2u);

  // Query forms in a vertex shader need no sample.
  w = NewModule();
  EmitEntry(w, 0u, 5u);
  EmitFunctionStart(w, 5u);
  Emit(w, 103u, {1u, 7u, 8u, 0u});
  EmitFunctionEnd(w);
  p = Prove(w, true);
  CHECK(p.safe);
  CHECK(p.samples == 0u);

  // Malformed query shapes, with a valid sample so only the query is wrong.
  w = NewModule();
  EmitEntry(w, 4u, 5u);
  EmitFunctionStart(w, 5u);
  Emit(w, 87u, {1u, 7u, 8u, 9u});
  Emit(w, 103u, {10u, 11u, 8u});  // OpImageQuerySizeLod needs a Lod word
  EmitFunctionEnd(w);
  p = Prove(w, false);
  CHECK(p.parsed);
  CHECK(!p.safe);
  CHECK((p.reasons & q::kProofBadImageForm) != 0u);

  w = NewModule();
  EmitEntry(w, 4u, 5u);
  EmitFunctionStart(w, 5u);
  Emit(w, 87u, {1u, 7u, 8u, 9u});
  Emit(w, 105u, {10u, 11u, 8u});  // OpImageQueryLod needs a coordinate
  EmitFunctionEnd(w);
  p = Prove(w, false);
  CHECK(!p.safe);
  CHECK((p.reasons & q::kProofBadImageForm) != 0u);

  w = NewModule();
  EmitEntry(w, 4u, 5u);
  EmitFunctionStart(w, 5u);
  Emit(w, 87u, {1u, 7u, 8u, 9u});
  Emit(w, 100u, {10u, 11u});  // OpImage needs the sampled image
  EmitFunctionEnd(w);
  p = Prove(w, false);
  CHECK(!p.safe);
  CHECK((p.reasons & q::kProofBadImageForm) != 0u);

  w = NewModule();
  EmitEntry(w, 4u, 5u);
  EmitFunctionStart(w, 5u);
  Emit(w, 87u, {1u, 7u, 8u, 9u});
  Emit(w, 106u, {10u, 11u, 8u, 12u});  // OpImageQueryLevels is 4 words
  EmitFunctionEnd(w);
  p = Prove(w, false);
  CHECK(!p.safe);
  CHECK((p.reasons & q::kProofBadImageForm) != 0u);
}

void TestDecision() {
  const q::SamplingQualityInfo eligible = EligibleInfo();

  // Mode 0 and any other mode are exact no-ops, including non-finite floats.
  for (uint8_t mode : {0u, 3u, 255u}) {
    const float min_lod = std::numeric_limits<float>::quiet_NaN();
    const float max_lod = 12.5f;
    const float bias = -1.5f;
    const auto decision =
        q::DecideSamplingQuality(mode, eligible, min_lod, max_lod, bias);
    CHECK(!decision.eligible);
    CHECK(!decision.changed);
    CHECK(decision.reasons == 0u);
    CHECK(Bits(decision.min_lod) == Bits(min_lod));
    CHECK(Bits(decision.max_lod) == Bits(max_lod));
    CHECK(Bits(decision.lod_bias) == Bits(bias));
  }

  // Mode 1 raises min Lod to 1 and leaves max/bias bits unchanged.
  const float max_lod = 12.5f;
  const float bias = -0.75f;
  auto decision = q::DecideSamplingQuality(1u, eligible, 0.0f, max_lod, bias);
  CHECK(decision.eligible);
  CHECK(decision.changed);
  CHECK(decision.reasons == 0u);
  CHECK(decision.min_lod == 1.0f);
  CHECK(Bits(decision.max_lod) == Bits(max_lod));
  CHECK(Bits(decision.lod_bias) == Bits(bias));

  // Mode 2 makes the same decision as mode 1.
  auto mode2 = q::DecideSamplingQuality(2u, eligible, 0.0f, max_lod, bias);
  CHECK(mode2.eligible);
  CHECK(mode2.changed);
  CHECK(Bits(mode2.min_lod) == Bits(decision.min_lod));
  CHECK(Bits(mode2.max_lod) == Bits(decision.max_lod));
  CHECK(Bits(mode2.lod_bias) == Bits(decision.lod_bias));

  // Idempotence: feeding the clamped result back changes nothing.
  auto again = q::DecideSamplingQuality(1u, eligible, decision.min_lod,
                                        decision.max_lod, decision.lod_bias);
  CHECK(again.eligible);
  CHECK(!again.changed);
  CHECK(again.reasons == 0u);
  CHECK(Bits(again.min_lod) == Bits(decision.min_lod));
  CHECK(Bits(again.max_lod) == Bits(decision.max_lod));
  CHECK(Bits(again.lod_bias) == Bits(decision.lod_bias));

  // A min Lod already above 1 is preserved bit-for-bit.
  auto high = q::DecideSamplingQuality(1u, eligible, 2.25f, 8.0f, 0.0f);
  CHECK(high.eligible);
  CHECK(!high.changed);
  CHECK(Bits(high.min_lod) == Bits(2.25f));

  // Acceptance boundaries.
  auto boundary = eligible;
  boundary.width = 256u;
  boundary.height = 256u;
  boundary.resident_levels = 2u;
  auto boundary_decision =
      q::DecideSamplingQuality(1u, boundary, 0.5f, 1.0f, 0.0f);
  CHECK(boundary_decision.eligible);
  CHECK(boundary_decision.changed);
  CHECK(boundary_decision.min_lod == 1.0f);

  auto min_one = q::DecideSamplingQuality(1u, eligible, 1.0f, 10.0f, 0.0f);
  CHECK(min_one.eligible);
  CHECK(!min_one.changed);
  CHECK(Bits(min_one.min_lod) == Bits(1.0f));

  // Every exclusion reason, one at a time.
  const auto expect_reason = [](q::SamplingQualityInfo info, uint32_t reason) {
    const auto d = q::DecideSamplingQuality(1u, info, 0.0f, 10.0f, 0.0f);
    CHECK(!d.eligible);
    CHECK(!d.changed);
    CHECK(d.reasons == reason);
    CHECK(Bits(d.min_lod) == Bits(0.0f));
    CHECK(Bits(d.max_lod) == Bits(10.0f));
  };

  auto info = eligible;
  info.known = false;
  expect_reason(info, q::kQualityNotKnown);

  info = eligible;
  info.ordinary_guest = false;
  expect_reason(info, q::kQualityNotOrdinaryGuest);

  info = eligible;
  info.sampled_only = false;
  expect_reason(info, q::kQualityNotSampledOnly);

  info = eligible;
  info.render_target = true;
  expect_reason(info, q::kQualityRenderTarget);

  info = eligible;
  info.resolve_target = true;
  expect_reason(info, q::kQualityResolveTarget);

  info = eligible;
  info.cpu_updated = true;
  expect_reason(info, q::kQualityCpuUpdated);

  info = eligible;
  info.late_upload = true;
  expect_reason(info, q::kQualityLateUpload);

  info = eligible;
  info.two_d = false;
  expect_reason(info, q::kQualityTwoDimensional);

  info = eligible;
  info.width = 0u;
  expect_reason(info, q::kQualityBadDimensions);

  info = eligible;
  info.width = 255u;
  expect_reason(info, q::kQualityBadDimensions);

  info = eligible;
  info.height = 255u;
  expect_reason(info, q::kQualityBadDimensions);

  info = eligible;
  info.base_level = 1u;
  expect_reason(info, q::kQualityBaseLevel);

  info = eligible;
  info.resident_levels = 1u;
  expect_reason(info, q::kQualityResidentLevels);

  info = eligible;
  info.resident_levels = 0u;
  expect_reason(info, q::kQualityResidentLevels);

  info = eligible;
  info.sampling_proof_safe = false;
  expect_reason(info, q::kQualityUnsafeSampling);

  info = eligible;
  auto nonfinite = q::DecideSamplingQuality(
      1u, info, std::numeric_limits<float>::quiet_NaN(), 10.0f, 0.0f);
  CHECK(!nonfinite.eligible);
  CHECK(nonfinite.reasons == q::kQualityNonFinite);

  nonfinite = q::DecideSamplingQuality(
      1u, info, 0.0f, std::numeric_limits<float>::infinity(), 0.0f);
  CHECK(!nonfinite.eligible);
  CHECK(nonfinite.reasons == q::kQualityNonFinite);

  nonfinite = q::DecideSamplingQuality(
      1u, info, 0.0f, 10.0f, -std::numeric_limits<float>::infinity());
  CHECK(!nonfinite.eligible);
  CHECK(nonfinite.reasons == q::kQualityNonFinite);

  auto below_one = q::DecideSamplingQuality(1u, eligible, 0.25f, 0.5f, 0.0f);
  CHECK(!below_one.eligible);
  CHECK(below_one.reasons == q::kQualityRangeBelowOne);

  auto inverted = q::DecideSamplingQuality(1u, eligible, 2.0f, 1.5f, 0.0f);
  CHECK(!inverted.eligible);
  CHECK(inverted.reasons == q::kQualityInvertedRange);

  // A rejected decision still reports unchanged float bits.
  CHECK(Bits(inverted.min_lod) == Bits(2.0f));
  CHECK(Bits(inverted.max_lod) == Bits(1.5f));
}

void TestHistory() {
  const q::RangeEndpoints range1{0x1000u, 0x2000u, 0x3000u, 0x4000u};
  const q::RangeEndpoints range2{0x5000u, 0x5100u, 0x6000u, 0x6100u};

  q::RangeWriteHistory history;
  auto observation = history.Observe(1u, range1, 7u);
  CHECK(observation.state == q::RangeObservation::kFirst);
  CHECK(observation.reasons == 0u);
  CHECK(observation.first_generation == 7u);
  CHECK(observation.observed);
  CHECK(history.size() == 1u);

  observation = history.Observe(1u, range1, 7u);
  CHECK(observation.state == q::RangeObservation::kStable);
  CHECK(observation.reasons == 0u);
  CHECK(observation.first_generation == 7u);
  CHECK(observation.observed);

  // A later generation change excludes permanently; the original generation
  // is never refreshed.
  observation = history.Observe(1u, range1, 8u);
  CHECK(observation.state == q::RangeObservation::kExcluded);
  CHECK((observation.reasons & q::kHistoryGeneration) != 0u);
  CHECK(observation.first_generation == 7u);
  CHECK(history.IsExcluded(1u));

  observation = history.Observe(1u, range1, 7u);
  CHECK(observation.state == q::RangeObservation::kExcluded);
  CHECK((observation.reasons & q::kHistoryGeneration) != 0u);
  CHECK(observation.first_generation == 7u);

  // A range identity change is also sticky.
  q::RangeWriteHistory range_history;
  CHECK(range_history.Observe(2u, range1, 3u).state ==
        q::RangeObservation::kFirst);
  observation = range_history.Observe(2u, range2, 3u);
  CHECK(observation.state == q::RangeObservation::kExcluded);
  CHECK((observation.reasons & q::kHistoryRange) != 0u);
  observation = range_history.Observe(2u, range1, 3u);
  CHECK(observation.state == q::RangeObservation::kExcluded);
  CHECK((observation.reasons & q::kHistoryRange) != 0u);

  q::RangeRecord record;
  CHECK(range_history.Find(2u, &record));
  CHECK(record.range == range1);
  CHECK(record.first_generation == 3u);
  CHECK((record.exclusion & q::kHistoryRange) != 0u);
  CHECK(record.observed);

  // A dirty watch after observation is sticky.
  q::RangeWriteHistory dirty_history;
  CHECK(dirty_history.Observe(3u, range1, 4u).state ==
        q::RangeObservation::kFirst);
  observation = dirty_history.MarkDirty(3u);
  CHECK(observation.state == q::RangeObservation::kExcluded);
  CHECK((observation.reasons & q::kHistoryCpuDirty) != 0u);
  observation = dirty_history.Observe(3u, range1, 4u);
  CHECK(observation.state == q::RangeObservation::kExcluded);
  CHECK((observation.reasons & q::kHistoryCpuDirty) != 0u);

  // A dirty watch before any observation creates a sticky placeholder.
  q::RangeWriteHistory early_history;
  observation = early_history.MarkDirty(9u);
  CHECK(observation.state == q::RangeObservation::kExcluded);
  CHECK(observation.reasons == q::kHistoryDirtyBeforeObservation);
  CHECK(!observation.observed);
  CHECK(early_history.size() == 1u);
  CHECK(early_history.IsExcluded(9u));
  observation = early_history.Observe(9u, range1, 5u);
  CHECK(observation.state == q::RangeObservation::kExcluded);
  CHECK(observation.observed);
  CHECK(observation.first_generation == 5u);
  CHECK((observation.reasons & q::kHistoryDirtyBeforeObservation) != 0u);
  CHECK(!early_history.IsExcluded(12345u));

  // Range-directed dirty marking only touches overlapping observed records.
  q::RangeWriteHistory overlap_history;
  const q::RangeEndpoints range3{0x9000u, 0x9100u, 0xA000u, 0xA100u};
  CHECK(overlap_history.Observe(1u, range1, 1u).state ==
        q::RangeObservation::kFirst);
  CHECK(overlap_history.Observe(2u, range2, 1u).state ==
        q::RangeObservation::kFirst);
  CHECK(overlap_history.Observe(3u, range3, 1u).state ==
        q::RangeObservation::kFirst);
  CHECK(overlap_history.MarkDirtyRange(0x1080u, 0x2080u) == 1u);
  CHECK(overlap_history.IsExcluded(1u));
  CHECK(!overlap_history.IsExcluded(2u));
  CHECK(!overlap_history.IsExcluded(3u));
  CHECK(overlap_history.MarkDirtyRange(0x6000u, 0x6010u) == 1u);
  CHECK(overlap_history.IsExcluded(2u));
  CHECK(!overlap_history.IsExcluded(3u));
  CHECK(overlap_history.MarkDirtyRange(0x9000u, 0x9000u) == 0u);
  CHECK(overlap_history.MarkDirtyRange(0xA100u, 0x9000u) == 0u);
  CHECK(!overlap_history.IsExcluded(3u));

  // A placeholder is not touched by a range wipe because it has no range yet.
  CHECK(overlap_history.MarkDirty(4u).state ==
        q::RangeObservation::kExcluded);
  CHECK(overlap_history.MarkDirtyRange(0u, 0xFFFFFFFFu) == 3u);
  CHECK(overlap_history.Find(4u, &record));
  CHECK(!record.observed);

  // Invalid endpoints on first observation record a sticky exclusion.
  q::RangeWriteHistory invalid_history;
  const q::RangeEndpoints invalid_range{0x2000u, 0x1000u, 0u, 0u};
  observation = invalid_history.Observe(7u, invalid_range, 1u);
  CHECK(observation.state == q::RangeObservation::kExcluded);
  CHECK((observation.reasons & q::kHistoryInvalidRange) != 0u);
  CHECK(observation.observed);
  CHECK(observation.first_generation == 1u);
  CHECK(invalid_history.Find(7u, &record));
  CHECK(record.range == invalid_range);
  CHECK((record.exclusion & q::kHistoryInvalidRange) != 0u);
  CHECK(!invalid_history.Find(8u, nullptr));
  CHECK(!invalid_history.IsExcluded(8u));

  // Capacity overflow is sticky and fail-closed for every key.
  q::RangeWriteHistory full_history;
  for (uint32_t i = 0u; i < q::RangeWriteHistory::kCapacity; ++i) {
    const q::RangeEndpoints range{
        i, i + 1u, i + 2u, i + 3u};
    CHECK(full_history.Observe(i, range, 1u).state ==
          q::RangeObservation::kFirst);
  }
  CHECK(full_history.size() == q::RangeWriteHistory::kCapacity);
  CHECK(!full_history.overflowed());

  observation = full_history.Observe(999999u, range1, 1u);
  CHECK(observation.state == q::RangeObservation::kOverflow);
  CHECK(observation.reasons == q::kHistoryOverflow);
  CHECK(full_history.overflowed());
  CHECK(full_history.size() == q::RangeWriteHistory::kCapacity);
  CHECK(full_history.IsExcluded(0u));
  CHECK(full_history.IsExcluded(999999u));

  observation = full_history.Observe(5u, range1, 1u);
  CHECK(observation.state == q::RangeObservation::kExcluded);
  CHECK((observation.reasons & q::kHistoryOverflow) != 0u);

  observation = full_history.MarkDirty(999998u);
  CHECK(observation.state == q::RangeObservation::kOverflow);
  CHECK(observation.reasons == q::kHistoryOverflow);

  // The dirty-first path fills the same capacity and overflows the same way.
  q::RangeWriteHistory placeholder_history;
  for (uint32_t i = 0u; i < q::RangeWriteHistory::kCapacity; ++i) {
    CHECK(placeholder_history.MarkDirty(100000u + i).state ==
          q::RangeObservation::kExcluded);
  }
  CHECK(!placeholder_history.overflowed());
  observation = placeholder_history.MarkDirty(999999u);
  CHECK(observation.state == q::RangeObservation::kOverflow);
  CHECK(placeholder_history.overflowed());
  CHECK(placeholder_history.IsExcluded(100000u));
}

void TestResolvePages() {
  q::ResolvePageMap map;
  CHECK(map.marked_pages() == 0u);
  CHECK(!map.IsPageMarked(0u));
  CHECK(!map.Overlaps(0u, q::ResolvePageMap::kPageBytes));

  CHECK(map.MarkRange(0u, 1u));
  CHECK(map.IsPageMarked(0u));
  CHECK(!map.IsPageMarked(1u));
  CHECK(map.marked_pages() == 1u);
  CHECK(map.Overlaps(0u, 1u));
  CHECK(map.Overlaps(1u, 1u));
  CHECK(!map.Overlaps(q::ResolvePageMap::kPageBytes, 1u));

  // A range spanning two pages marks both.
  CHECK(map.MarkRange(q::ResolvePageMap::kPageBytes - 1u, 2u));
  CHECK(map.IsPageMarked(1u));
  CHECK(map.marked_pages() == 2u);
  CHECK(map.Overlaps(q::ResolvePageMap::kPageBytes, 1u));

  // The last guest page can be marked, but nothing beyond the 512 MiB range.
  CHECK(map.MarkRange(q::ResolvePageMap::kGuestBytes - 1u, 1u));
  CHECK(map.IsPageMarked(q::ResolvePageMap::kPageCount - 1u));
  CHECK(!map.MarkRange(q::ResolvePageMap::kGuestBytes, 1u));
  CHECK(!map.MarkRange(q::ResolvePageMap::kGuestBytes - 1u, 2u));
  CHECK(!map.MarkRange(0xFFFFFFFFu, 2u));
  CHECK(map.MarkRange(0u, 0u));
  CHECK(map.marked_pages() == 3u);

  // Out-of-range overlap queries clamp rather than read past the bitmap.
  CHECK(!map.Overlaps(q::ResolvePageMap::kGuestBytes, 4096u));
  CHECK(map.Overlaps(0u, 0xFFFFFFFFu));
  CHECK(!map.Overlaps(0u, 0u));

  // Endpoint marking covers both halves and checks everything first.
  q::ResolvePageMap endpoints;
  const q::RangeEndpoints range{0x1000u, 0x2000u, 0x3000u, 0x3001u};
  CHECK(endpoints.MarkEndpoints(range));
  CHECK(endpoints.IsPageMarked(1u));
  CHECK(endpoints.IsPageMarked(3u));
  CHECK(endpoints.marked_pages() == 2u);
  CHECK(endpoints.OverlapsEndpoints(range));
  CHECK(!endpoints.OverlapsEndpoints(
      q::RangeEndpoints{0x4000u, 0x5000u, 0x6000u, 0x7000u}));
  CHECK(endpoints.OverlapsEndpoints(
      q::RangeEndpoints{0x1FFFu, 0x2000u, 0u, 0u}));

  q::ResolvePageMap rejected;
  CHECK(!rejected.MarkEndpoints(
      q::RangeEndpoints{q::ResolvePageMap::kGuestBytes,
                        q::ResolvePageMap::kGuestBytes, 0u, 1u}));
  CHECK(rejected.marked_pages() == 0u);
  CHECK(!rejected.MarkEndpoints(
      q::RangeEndpoints{0x2000u, 0x1000u, 0u, 0u}));
  CHECK(rejected.marked_pages() == 0u);
  CHECK(rejected.OverlapsEndpoints(
      q::RangeEndpoints{0x2000u, 0x1000u, 0u, 0u}));

  // Empty half-ranges are valid and mark nothing.
  CHECK(rejected.MarkEndpoints(
      q::RangeEndpoints{0x1000u, 0x1000u, 0x2000u, 0x2000u}));
  CHECK(rejected.marked_pages() == 0u);

  // A texture range may overlap resolved pages without changing history, and
  // history exclusions do not mark pages.
  q::RangeWriteHistory history;
  const q::RangeEndpoints texture{0x1FFFF000u, 0x20000000u,
                                  0x1FFFF800u, 0x20000000u};
  CHECK(history.Observe(42u, texture, 5u).state ==
        q::RangeObservation::kFirst);
  q::ResolvePageMap overlap;
  CHECK(overlap.MarkEndpoints(texture));
  CHECK(overlap.OverlapsEndpoints(texture));
  CHECK(!history.IsExcluded(42u));
  CHECK(!overlap.OverlapsEndpoints(
      q::RangeEndpoints{0u, 0x1000u, 0u, 0x0u}));
}

int AnalyzeCorpusFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "CORPUS %s: cannot open\n", path);
    return 1;
  }
  const std::vector<uint8_t> bytes(
      (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto proof =
      q::ProveSpirvSampling(bytes.data(), bytes.size(), false);
  std::printf(
      "CORPUS %s bytes=%zu parsed=%d safe=%d samples=%u reasons=0x%08x\n",
      path, bytes.size(), proof.parsed ? 1 : 0, proof.safe ? 1 : 0,
      proof.samples, proof.reasons);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  TestParseMode();
  TestProofHeaderAndFraming();
  TestProofImageForms();
  TestDecision();
  TestHistory();
  TestResolvePages();

  // Director review regressions: image atomics are exact image accesses too;
  // the 64-bit write-generation sum must not truncate to one page's width.
  const auto texel_pointer = Prove(FragmentModuleWith(60u, {1u, 7u, 8u, 9u, 10u}), false);
  CHECK(!texel_pointer.safe);
  CHECK((texel_pointer.reasons & q::kProofForbiddenImageOp) != 0);
  q::RangeWriteHistory wide_generation;
  const q::RangeEndpoints wide_range{0x1000, 0x2000, 0, 0};
  CHECK(wide_generation.Observe(1, wide_range, 7).state == q::RangeObservation::kFirst);
  CHECK(wide_generation.Observe(1, wide_range, (uint64_t{1} << 32) + 7).state == q::RangeObservation::kExcluded);
  Words unknown_import = NewModule();
  Emit(unknown_import, 11u, {15u, 0x00646162u});  // unproved extended set "bad"
  EmitEntry(unknown_import, 4u, 5u);
  EmitFunctionStart(unknown_import, 5u);
  Emit(unknown_import, 87u, {1u, 7u, 8u, 9u});
  EmitFunctionEnd(unknown_import);
  CHECK(!Prove(unknown_import, false).safe);

  int corpus_status = 0;
  for (int i = 1; i < argc; ++i) {
    if (AnalyzeCorpusFile(argv[i]) != 0) corpus_status = 2;
  }

  std::printf("checks=%d failures=%d\n", g_checks, g_failures);
  if (g_failures != 0) return 1;
  return corpus_status;
}
