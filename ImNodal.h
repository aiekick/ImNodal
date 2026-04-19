/*
MIT License

Copyright (c) 2025-2026 Stephane Cuillerdier (aka Aiekick)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

// ImNodal, v0.1.0
// Zoomable/pannable canvas + node graph primitives for Dear ImGui.
// This v0.1 ships the Canvas base only; graph API lands after.
//
// Canvas is inspired by thedmd/imgui-node-editor's ImCanvas: local-space
// transform lets ImGui widgets scale with the zoom level.

#define IMNODAL_VERSION "0.1.0"
#define IMNODAL_VERSION_NUM 00100

#ifndef IMNODAL_API
#define IMNODAL_API
#endif

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#ifdef IMGUI_INCLUDE
#include IMGUI_INCLUDE
#else
#include <imgui_internal.h>
#endif

namespace ImNodal {

// =====================================================================
// Context
// =====================================================================
// Every canvas owns a Context (view state, interaction state, etc.).
// The "current" context is an implicit argument to all Begin/End/queries.
// Typical lifecycle:
//   auto* ctx = ImNodal::CreateContext();
//   ImNodal::SetCurrentContext(ctx);
//   ... each frame: BeginCanvas/EndCanvas + queries ...
//   ImNodal::DestroyContext(ctx);
//
// Multiple contexts can coexist — switch between them with SetCurrentContext
// before the matching Begin/End pair.

struct Context;  // opaque

IMNODAL_API Context* CreateContext();
IMNODAL_API void     DestroyContext(Context* apCtx = nullptr);  // null = destroy current
IMNODAL_API Context* GetCurrentContext();
IMNODAL_API void     SetCurrentContext(Context* apCtx);

// =====================================================================
// Canvas
// =====================================================================

struct CanvasSettings {
    // Zoom
    float zoomStep{0.1f};                                      // Amount added/removed per wheel tick
    float zoomMin{0.1f};                                       // Minimum scale
    float zoomMax{10.0f};                                      // Maximum scale
    ImGuiKey resetZoomKey{ImGuiKey_R};                         // Key to reset zoom to 1.0 (ImGuiKey_None to disable)

    // Pan
    ImGuiMouseButton panButton{ImGuiMouseButton_Middle};       // Button used to drag the canvas

    // Context menu
    ImGuiMouseButton contextMenuButton{ImGuiMouseButton_Right};// Button triggering background context menu request

    // Grid
    bool drawGrid{true};                                       // Auto-draw grid during Begin (can also be called manually)
    ImVec2 gridSize{50.0f, 50.0f};                             // Major grid spacing in canvas units
    ImVec2 gridSubdivs{5.0f, 5.0f};                            // Minor subdivisions per major cell (0 to disable)
    ImU32 gridColor{IM_COL32(200, 200, 200, 40)};
    ImU32 subGridColor{IM_COL32(200, 200, 200, 10)};

    CanvasSettings() = default;
};

// Check that compiled-in struct size matches the caller. Use IMNODAL_CHECKVERSION().
#define IMNODAL_CHECKVERSION() ImNodal::DebugCheckVersion(IMNODAL_VERSION, sizeof(ImNodal::CanvasSettings))
IMNODAL_API bool DebugCheckVersion(const char* aVersion, size_t aSettingsSize);

// -----------------------------
// Begin / End
// -----------------------------
// aSize: pass (0,0) to use the remaining content region.
// Returns false if the canvas is fully clipped; do NOT call EndCanvas() in that case.
IMNODAL_API bool BeginCanvas(const char* aId, const ImVec2& aSize = ImVec2(0.0f, 0.0f), const CanvasSettings& arSettings = {});
IMNODAL_API void EndCanvas();

// -----------------------------
// Queries (valid during and immediately after EndCanvas of the current frame)
// -----------------------------
IMNODAL_API bool IsCanvasHovered();                      // Mouse is over the canvas widget rect
IMNODAL_API bool IsCanvasBackgroundClicked();            // Left click on empty canvas (no ImGui item under cursor)
IMNODAL_API bool IsCanvasBackgroundDoubleClicked();      // Left double-click on empty canvas
IMNODAL_API bool IsCanvasContextMenuRequested();         // contextMenuButton clicked on empty canvas
IMNODAL_API bool IsCanvasPanning();                      // Currently dragging with panButton

// -----------------------------
// View (origin in screen pixels, scale is uniform)
// -----------------------------
IMNODAL_API ImVec2 GetCanvasOrigin();
IMNODAL_API float  GetCanvasScale();
IMNODAL_API void   SetCanvasView(const ImVec2& aOrigin, float aScale);
IMNODAL_API void   ResetCanvasView();                                    // origin = widget center, scale = 1
IMNODAL_API void   CenterCanvasOn(const ImVec2& aCanvasPos);             // Keep current scale, recenter on canvas point
IMNODAL_API void   ZoomCanvasToRect(const ImVec2& aMin, const ImVec2& aMax, float aMarginRatio = 0.1f);

// -----------------------------
// Coordinate conversion
// -----------------------------
IMNODAL_API ImVec2 CanvasToScreen(const ImVec2& aP);     // Point (includes origin offset)
IMNODAL_API ImVec2 ScreenToCanvas(const ImVec2& aP);
IMNODAL_API ImVec2 CanvasToScreenV(const ImVec2& aV);    // Vector (scale only, no translation)
IMNODAL_API ImVec2 ScreenToCanvasV(const ImVec2& aV);

// -----------------------------
// Rects
// -----------------------------
IMNODAL_API ImRect GetCanvasRect();         // Canvas widget rect in screen space
IMNODAL_API ImRect GetCanvasViewRect();     // Visible area expressed in canvas space

// -----------------------------
// Escape hatch: draw at screen scale while inside a Begin/End scope.
// Useful for overlays (minimap, HUD) that must not zoom with the canvas.
// Must be matched. SuspendCanvas/ResumeCanvas can nest.
// -----------------------------
IMNODAL_API void SuspendCanvas();
IMNODAL_API void ResumeCanvas();
IMNODAL_API bool IsCanvasSuspended();

// -----------------------------
// Manual grid draw (only needed if you disabled CanvasSettings::drawGrid).
// Must be called between Begin and End.
// -----------------------------
IMNODAL_API void DrawCanvasGrid();

}  // namespace ImNodal
