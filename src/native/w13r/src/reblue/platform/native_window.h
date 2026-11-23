/**
 * @file    platform/native_window.h
 * @brief   Build a plume::RenderWindow swapchain target from the app window.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <plume_render_interface.h>

namespace rex::ui {
class Window;
}

namespace bd::platform {

// Fill 'out' with the swapchain surface target for 'window'. The window is
// the single SDK SDL3 window on every platform. Only the handle plume needs
// differs, since its RenderWindow type is per-platform: a Win32 HWND, an
// NSWindow plus CAMetalLayer, or the SDL window itself on Linux. Returns false
// and logs when that handle is unavailable.
bool GetNativeRenderWindow(rex::ui::Window *window, plume::RenderWindow &out);

} // namespace bd::platform
