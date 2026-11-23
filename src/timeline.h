// Light, readable session timeline (card LOG1). A small text file logs/wwe2k14-timeline-<stamp>.txt next to the main
// log: a header (build, renderer, the launcher settings of this run) and one line per event, "[hh:mm:ss] perf ...",
// "files PAC\MENU", "HITCH", "PAUSE", "STUCK?", "FREEZE", "CRASH", "EXIT". Notes go into a fixed lock-free ring in
// memory; a background thread rewrites the file every 10 s and at freeze/stuck/exit; a crash writes it from the
// handler. The newest 5 timeline files are kept. WWE2K14_TIMELINE=0 turns everything off (nothing is registered).
#pragma once

#include <cstdint>

namespace wwe2k14::timeline {

// Once, early (app setup). directory: the game's logs folder; settings_file: the launcher's wwe2k14-enhanced.ini
// (listed in the header; may be missing). Also registers the SDK file-open / input / crash observers and (Windows)
// an unhandled-exception filter.
void Start(const char* directory, const char* settings_file);

// printf-style note, e.g. Note("event", "EXIT"). Cheap (no lock, no allocation); text is truncated to one line.
void Note(const char* category, const char* fmt, ...);

// Once per game flip (the sub_82CB6318 hook). Builds the 10 s "perf" notes and the HITCH / PAUSE notes.
void OnPresent();

// The SDK freeze report was written: notes FREEZE and writes the file, but only when the timeline itself saw no
// frame for over 5 s (the SDK logger has false reports while frames continue; those add nothing here).
void OnFreezeReport();

// Writes the file now on the calling thread (window close). Cheap (a few KB) and safe to call repeatedly.
void Flush();

// Stops the writer thread (woken at once), unregisters the file/input observers and writes the file a last time.
void Stop();

}  // namespace wwe2k14::timeline
