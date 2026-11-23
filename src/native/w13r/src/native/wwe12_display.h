// WWE '12 PC: display options that live in the game process (wwe12_display.cpp): FPS counter (F2),
// frame limit, CPU priority.
#pragma once

namespace rex::ui {
class ImGuiDrawer;
}

// Called once by the app, on the UI thread, when ImGui can draw: applies the CPU priority, binds F2
// and shows the counter if wwe12_show_fps is on.
void Wwe12DisplayInit(rex::ui::ImGuiDrawer *drawer);

// Called once per presented frame (SvrOnFrame): counts it for the FPS counter and, if
// wwe12_fps_limit is set, waits so frames come no faster than the limit.
void Wwe12DisplayOnFrame();
