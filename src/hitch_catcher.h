// Stall catcher (test builds; tools/diag/triage.py reads its output). When no game frame is presented for
// WWE2K14_HITCH_MS (default 150) ms, a watchdog thread samples every thread of the process every 10 ms until the next
// present: leaf PC, executable return-address candidates from the stack, CPU time. One [hitch] header line plus one
// line per busy or in-game thread is logged when the stall ends. Windows only; elsewhere the calls are no-ops.
// WWE2K14_HITCH=0 turns it off. Every build logs "[hitch] build=<pdb guid+age>" once so logs match out/symbols/.
#pragma once

#include <string>

namespace wwe2k14::hitch {

void Start();        // idempotent; first guest present
void NotePresent();  // every guest present (cheap: one relaxed store)

// Build id of the running executable: Windows = PDB GUID + age (the "[hitch] build=" id), Linux = GNU build-id hex,
// "unknown" when the binary carries none. Not cheap: call once.
std::string BuildId();

}  // namespace wwe2k14::hitch
