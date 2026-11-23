#include "native/nr/fix_alt.h"
#include "native/nr/fix_alt_knobs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int failures = 0;

void Check(bool ok, const char* name) {
  std::printf("check=%s result=%s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) ++failures;
}

void SetEnv(const char* name, const char* value) {
  if (value) setenv(name, value, 1);
  else unsetenv(name);
}
}  // namespace

int main() {
  using nr::backend::fix_alt::ParseSeconds;
  using nr::backend::fix_alt::Scheduler;

  Check(ParseSeconds(nullptr) == 0, "scheduler_missing_off");
  Check(ParseSeconds("") == 0, "scheduler_empty_off");
  Check(ParseSeconds("0") == 0 && ParseSeconds("-3") == 0, "scheduler_nonpositive_off");
  Check(ParseSeconds("3") == 3, "scheduler_seconds_parsed");
  Check(ParseSeconds("3x") == 0 && ParseSeconds("99999999999999999999") == 0,
        "scheduler_rejects_trailing_and_overflow");

  Scheduler scheduler(2);
  Check(scheduler.enabled() && scheduler.period_seconds() == 2 && !scheduler.phase_b(), "scheduler_starts_in_a");
  Check(scheduler.Tick(100) == -1 && scheduler.Tick(1999999999ull) == -1, "scheduler_does_not_flip_early");
  Check(scheduler.Tick(2000000100ull) == 1 && scheduler.phase_b(), "scheduler_flips_to_b_at_period");
  Check(scheduler.Tick(4000000100ull) == 0 && !scheduler.phase_b(), "scheduler_flips_back_to_a");
  Scheduler disabled(0);
  Check(disabled.Tick(9000000000ull) == -1 && !disabled.enabled(), "scheduler_disabled_never_flips");

  SetEnv("WWE2K14_NR_RETIRE_FRAMES", "2");
  SetEnv("WWE2K14_LOW_POWER", "1");
  SetEnv("WWE2K14_NR_EARLY_SUBMIT", "256");
  SetEnv("WWE2K14_D3D_CROWD_GROUP", "0");
  SetEnv("WWE2K14_NR_FIX_ALT", nullptr);
  SetEnv("WWE2K14_NR_FIX_ALT_B",
         " WWE2K14_NR_RETIRE_FRAMES = 3 ; ;invalid;WWE2K14_LOW_POWER=0;WWE2K14_NR_EARLY_SUBMIT=300;"
         "WWE2K14_NR_RETIRE_FRAMES=4;WWE2K14_LOW_POWER=1tail;=2;WWE2K14_UNKNOWN=7;WWE2K14_D3D_CROWD_GROUP=1");

  nr::backend::SetFixAltPhaseB(false);
  const nr::backend::FixAltKnob retire_frames = nr::backend::FixAltKnobFor("WWE2K14_NR_RETIRE_FRAMES");
  const nr::backend::FixAltKnob low_power = nr::backend::FixAltKnobFor("WWE2K14_LOW_POWER");
  const nr::backend::FixAltKnob early_submit = nr::backend::FixAltKnobFor("WWE2K14_NR_EARLY_SUBMIT");
  const nr::backend::FixAltKnob crowd_group = nr::backend::FixAltKnobFor("WWE2K14_D3D_CROWD_GROUP");
  const nr::backend::FixAltKnob missing = nr::backend::FixAltKnobFor("WWE2K14_MISSING_KNOB");
  Check(nr::backend::FixAltInt(retire_frames, 2) == 2, "knob_phase_a_normal_env");
  Check(nr::backend::FixAltInt(low_power, 0) == 1, "knob_boolean_normal_env");
  Check(nr::backend::FixAltInt(early_submit, 0) == 256, "knob_normal_launcher_value");
  Check(nr::backend::FixAltInt(crowd_group, 0) == 0, "crowd_group_phase_a_off");
  Check(nr::backend::FixAltInt(missing, 19) == 19, "knob_missing_uses_default");

  // The normal values are snapshotted on first access; later environment edits do not change A.
  SetEnv("WWE2K14_NR_RETIRE_FRAMES", "99");
  Check(nr::backend::FixAltInt(retire_frames, 2) == 2, "knob_normal_value_cached");

  nr::backend::SetFixAltPhaseB(true);
  Check(nr::backend::FixAltInt(retire_frames, 2) == 4, "knob_b_duplicate_last_valid_wins");
  Check(nr::backend::FixAltInt(low_power, 0) == 0, "knob_invalid_b_value_ignored");
  Check(nr::backend::FixAltInt(early_submit, 0) == 300, "knob_b_early_submit_value");
  Check(nr::backend::FixAltInt(crowd_group, 0) == 1, "crowd_group_phase_b_on");
  Check(nr::backend::FixAltInt(missing, 19) == 19, "knob_unlisted_uses_normal_default");
  nr::backend::SetFixAltPhaseB(false);
  Check(nr::backend::FixAltInt(retire_frames, 2) == 2, "knob_phase_returns_to_a");

  std::printf("fix_alt_test failures=%d\n", failures);
  return failures ? 1 : 0;
}
