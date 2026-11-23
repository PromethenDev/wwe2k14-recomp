#include <cassert>

#include "native/d3d/weakpc_options.h"

using namespace wwe2k14::d3d::weakpc;

int main() {
  assert(ParseCrowdMode(nullptr) == 1);
  assert(ParseCrowdMode("1") == 1);
  assert(ParseCrowdMode("0") == 0);
  assert(ParseCrowdMode("2") == 2);
  assert(ParseCrowdMode("invalid") == 1);
  assert(ParseShadowsEnabled(nullptr));
  assert(ParseShadowsEnabled("1"));
  assert(!ParseShadowsEnabled("0"));
  assert(ParseShadowsEnabled("invalid"));

  CrowdDrawCadence cadence;
  assert(!DropCrowdDraw(0, 0, 10, &cadence));
  assert(DropCrowdDraw(0, kCrowdPixelShader, 10, &cadence));
  assert(!DropCrowdDraw(1, kCrowdPixelShader, 10, &cadence));
  assert(!DropCrowdDraw(2, kCrowdPixelShader, 10, &cadence));
  assert(DropCrowdDraw(2, kCrowdPixelShader, 10, &cadence));
  assert(!DropCrowdDraw(2, kCrowdPixelShader, 10, &cadence));
  assert(!DropCrowdDraw(2, kCrowdPixelShader, 11, &cadence));  // cadence restarts each frame
  assert(DropCrowdDraw(2, kCrowdPixelShader, 11, &cadence));

  assert(IsFixed16ShadowTarget(560, 512, false, true));
  assert(!IsFixed16ShadowTarget(560, 512, true, true));
  assert(!IsFixed16ShadowTarget(559, 512, false, true));
  assert(!IsFixed16ShadowTarget(560, 512, false, false));
  assert(IsShadowPassPixelShader(0xACF4C56B7294FFFCull));
  assert(IsShadowPassPixelShader(0xE17C541F03AF4646ull));
  assert(IsShadowPassPixelShader(0x7A0DFD92C5D0ADF3ull));
  assert(IsShadowPassPixelShader(0x2FC3888D607DEB97ull));
  assert(!IsShadowPassPixelShader(0x12345678ull));

  assert(IsShadowInput(kF1ShadowConsumerLinear, 0, 0x1FA00000u));
  assert(IsShadowInput(kF1ShadowConsumerPoint, 0, 0x1FA00000u));
  assert(IsShadowInput(0, 12, 0x1FA00000u));
  assert(IsShadowInput(0, 12, 0x1F600000u));
  assert(IsShadowInput(0, 0, 0x1F800000u));
  assert(!IsShadowInput(0x1234, 0, 0x1FA00000u));
  assert(!IsShadowInput(0, 12, 0x12345678u));
  assert(!IsShadowInput(0, 11, 0x1F600000u));
  return 0;
}
