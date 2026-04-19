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

#include <cstddef>
#include <cstdint>

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

// Must be called once per frame on the current context, BEFORE any other
// ImNodal call this frame (same contract as ImGui::NewFrame). Clears per-frame
// state: hovered slot/link flags, connection-create transient flags, stale
// drag state left behind by a missing EndConnectionCreate. Calling it more
// than once per frame is a no-op.
IMNODAL_API void     NewFrame();

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

// =====================================================================
// Graph layer (M1: skeleton — Graph + Node + Slot primitive + sections)
// =====================================================================
// Design philosophy: the slot is the primitive. Nodes are intelligent
// containers that pin slots on their edges. A slot can be emitted ANYWHERE
// (inside a node section, inside a plain ImGui window, inline with text...).
//
// Usage (M1):
//
//     if (BeginCanvas("c", size, canvasSettings)) {
//         if (BeginGraph(graphId, graphSettings)) {
//             if (BeginNode(nodeId, &pos, nodeSettings)) {
//                 if (BeginHeader())  { ImGui::Text("Title"); EndHeader(); }
//                 if (BeginInputs())  {
//                     if (BeginInputSlot(slotId, "A")) { /* widgets */ EndSlot(); }
//                     EndInputs();
//                 }
//                 if (BeginCenter())  { /* body widget */ EndCenter(); }
//                 if (BeginOutputs()) {
//                     if (BeginOutputSlot(slotId2, "Out")) { EndSlot(); }
//                     EndOutputs();
//                 }
//                 if (BeginFooter()) { /* widgets */ EndFooter(); }
//                 EndNode();
//             }
//             EndGraph();
//         }
//         EndCanvas();
//     }
//
// BeginGraph must be called INSIDE a BeginCanvas scope.

using Id = uint64_t;  // ImNodal::Id — user-chosen, must be non-zero

enum SlotRole_ {
    SlotRole_Input  = 0,
    SlotRole_Output = 1,
    SlotRole_InOut  = 2,  // bidirectional (reroute / bridge slots) — accepts links either way
};
typedef int SlotRole;

// -----------------------------
// Graph
// -----------------------------
struct GraphSettings {
    bool allowBoxSelect{true};
    bool allowMultiSelect{true};
    ImGuiKey multiSelectKey{ImGuiMod_Shift};
    ImGuiMouseButton selectButton{ImGuiMouseButton_Left};
    ImGuiMouseButton dragButton{ImGuiMouseButton_Left};
    float minSlotHitRadiusScreen{8.0f};  // keeps slots clickable when zoomed out
    GraphSettings() = default;
};

IMNODAL_API bool BeginGraph(Id aGraphId, const GraphSettings& arSettings = {});
IMNODAL_API void EndGraph();
IMNODAL_API Id   GetCurrentGraphId();

// -----------------------------
// Node
// -----------------------------
struct NodeSettings {
    ImU32 headerColor{IM_COL32(60, 120, 180, 255)};
    ImU32 bodyColor{IM_COL32(50, 50, 50, 230)};
    ImU32 borderColor{IM_COL32(80, 80, 80, 255)};
    ImU32 selectedBorderColor{IM_COL32(255, 180, 0, 255)};
    ImU32 titleColor{IM_COL32(255, 255, 255, 255)};
    ImU32 hoverHandleColor{IM_COL32(255, 255, 255, 120)};   // drawn when drawHoverHandle && hovered
    float rounding{4.0f};
    float borderThickness{1.5f};
    float headerPadding{6.0f};
    float bodyPadding{6.0f};
    float columnSpacing{10.0f};   // between Inputs/Center/Outputs
    float hoverHandleHeight{4.0f}; // height of the hover-only drag bar (reroute-style nodes)
    bool  movable{true};
    bool  hasInnerGraph{false};
    bool  drawHoverHandle{false}; // draw a drag bar on top of the node when hovered (reroute-style nodes)
    NodeSettings() = default;
};

// aPos is IN/OUT in canvas space. When non-null and movable, dragging updates it.
IMNODAL_API bool BeginNode(Id aNodeId, ImVec2* apPos, const NodeSettings& arSettings = {});
IMNODAL_API void EndNode();

// Optional layout sections. None are mandatory. Body = Inputs | Center | Outputs
// organized in 3 columns via SameLine; Header above body; Footer below.
IMNODAL_API bool BeginHeader();  IMNODAL_API void EndHeader();
IMNODAL_API bool BeginInputs();  IMNODAL_API void EndInputs();
IMNODAL_API bool BeginCenter();  IMNODAL_API void EndCenter();
IMNODAL_API bool BeginOutputs(); IMNODAL_API void EndOutputs();
IMNODAL_API bool BeginFooter();  IMNODAL_API void EndFooter();

// -----------------------------
// Slot (primitive — usable anywhere)
// -----------------------------
struct SlotSettings {
    ImU32 dotColor{IM_COL32(200, 200, 200, 255)};
    ImU32 dotColorConnected{IM_COL32(255, 220, 0, 255)};
    ImU32 dotColorHovered{IM_COL32(255, 255, 255, 255)};
    float dotRadius{5.0f};
    uint32_t typeTag{0};   // user-chosen type tag (for M2 connection rules)
    SlotSettings() = default;
};

// Render a slot. Positioning behavior depends on the current "pin mode":
//   - inside BeginInputs()  → dot pinned to the node's left edge
//   - inside BeginOutputs() → dot pinned to the node's right edge
//   - anywhere else         → dot inline next to the label/widget
//
// Dot Y is always centered on the group rect of everything emitted between
// BeginSlot and EndSlot.
IMNODAL_API bool BeginSlot(Id aSlotId, SlotRole aRole, const char* aLabel, const SlotSettings& arSettings = {});
IMNODAL_API void EndSlot();
// Convenience wrappers
IMNODAL_API bool BeginInputSlot (Id aSlotId, const char* aLabel, const SlotSettings& arSettings = {});
IMNODAL_API bool BeginOutputSlot(Id aSlotId, const char* aLabel, const SlotSettings& arSettings = {});

// -----------------------------
// Graph queries & interactions
// -----------------------------
IMNODAL_API ImVec2 GetSlotScreenPos(Id aSlotId);
IMNODAL_API ImVec2 GetSlotTangent(Id aSlotId);         // unit vector pointing away from the node edge
IMNODAL_API bool   IsSlotHovered(Id aSlotId);
IMNODAL_API bool   IsSlotConnected(Id aSlotId);

IMNODAL_API bool   IsNodeHovered(Id* apoNodeId);        // set *apoNodeId to hovered node id if any
IMNODAL_API bool   IsNodeSelected(Id aNodeId);
IMNODAL_API Id     GetSelectedNode();                    // 0 if none (M1: single selection only)
IMNODAL_API void   SetSelectedNode(Id aNodeId);          // pass 0 to clear
IMNODAL_API bool   IsNodeDragging(Id aNodeId);

// =====================================================================
// Links (M2)
// =====================================================================
// Default render is a cubic Bezier. Colors default to a neutral white when
// aColor==0. Must be called inside BeginGraph/EndGraph, after the nodes that
// own the slots have been emitted (otherwise slot screen positions are stale).

IMNODAL_API void Link(Id aLinkId, Id aFromSlotId, Id aToSlotId, ImU32 aColor = 0, float aThickness = 3.0f);

// Per-link queries (valid after the matching Link() call in the same frame).
IMNODAL_API bool   IsLinkHovered(Id aLinkId);
IMNODAL_API bool   IsLinkClicked(Id aLinkId, int aButton = 0);
IMNODAL_API bool   IsLinkDoubleClicked(Id aLinkId);
IMNODAL_API bool   IsLinkSelected(Id aLinkId);
IMNODAL_API Id     GetSelectedLink();
IMNODAL_API void   SetSelectedLink(Id aLinkId);            // pass 0 to clear
IMNODAL_API SlotRole GetSlotRole(Id aSlotId);              // useful for rule checks during link creation

// =====================================================================
// Connection creation (M2 — thedmd-style state machine)
// =====================================================================
// Typical usage inside BeginGraph/EndGraph, AFTER nodes + links are declared:
//
//     if (BeginConnectionCreate()) {
//         Id from, to;
//         if (QueryNewLink(&from, &to)) {
//             if (rules_ok(from, to)) {
//                 if (AcceptNewLink()) { commit_link(from, to); }
//             } else {
//                 RejectNewLink("type mismatch");
//             }
//         }
//         EndConnectionCreate();
//     }
//
// BeginConnectionCreate returns true as soon as the user starts dragging from
// a slot. QueryNewLink returns true every frame while a compatible target slot
// is hovered. AcceptNewLink lights the preview green and returns true once on
// the frame the mouse is released (commit). RejectNewLink paints it red.

IMNODAL_API bool BeginConnectionCreate();
IMNODAL_API bool QueryNewLink(Id* apoFromSlotId, Id* apoToSlotId);
IMNODAL_API bool AcceptNewLink(ImU32 aColor = 0);
IMNODAL_API void RejectNewLink(const char* aReason = nullptr);
IMNODAL_API void EndConnectionCreate();

// =====================================================================
// Reroute node (M2)
// =====================================================================
// A minimal pass-through node: no header/body/footer, just an input and an
// output slot at the same point. Used to bend links. Still draggable; clicking
// + dragging from either slot starts a new connection.

// A reroute has exactly one slot (SlotRole_InOut) that serves as both input
// and output. The node shows a hover-only drag handle bar (visible only when
// the mouse is over the node) so you can move it around. Typically paired
// with double-click-on-link to split a link at a given point.
IMNODAL_API bool BeginRerouteNode(Id aNodeId, Id aSlotId, ImVec2* apPos, const NodeSettings& arSettings = {});
IMNODAL_API void EndRerouteNode();

}  // namespace ImNodal
