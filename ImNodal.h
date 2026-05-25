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

enum ImNodalCol_ {
    ImNodalCol_GridLine = 0,
    ImNodalCol_GridSubLine,
    ImNodalCol_NodeBody,
    ImNodalCol_NodeBorder,
    ImNodalCol_NodeBorderSelected,
    ImNodalCol_NodeHoverHandle,
    ImNodalCol_Link,  // default link color (when host passes 0)
    ImNodalCol_LinkHovered,
    ImNodalCol_LinkSelected,
    ImNodalCol_BoxSelectFill,
    ImNodalCol_BoxSelectBorder,
    ImNodalCol_LinkPreviewIdle,    // wire color while dragging, no target hovered
    ImNodalCol_LinkPreviewAccept,  // wire color when AcceptNewLink fired
    ImNodalCol_LinkPreviewReject,  // wire color when RejectNewLink fired
    ImNodalCol_FlowDot,            // moving dot color along FlowLink
    ImNodalCol_MiniMapBg,          // minimap background fill
    ImNodalCol_MiniMapBorder,      // minimap outer frame
    ImNodalCol_MiniMapNode,        // default fill for a node rect inside the minimap (used when NodeSettings::miniMapColor==0)
    ImNodalCol_MiniMapViewport,    // rect outlining the visible canvas inside the minimap
    ImNodalCol_COUNT,
};
typedef int ImNodalCol;

// Anchor corner for ShowMiniMap.
enum ImNodalCorner_ {
    ImNodalCorner_TopLeft = 0,
    ImNodalCorner_TopRight,
    ImNodalCorner_BottomLeft,
    ImNodalCorner_BottomRight,
};
typedef int ImNodalCorner;

enum ImNodalStyleVar_ {
    ImNodalStyleVar_NodeRounding = 0,       // float
    ImNodalStyleVar_NodeBorderThickness,    // float
    ImNodalStyleVar_NodeBodyPadding,        // float
    ImNodalStyleVar_NodeHoverHandleHeight,  // float
    ImNodalStyleVar_SlotMinSize,            // ImVec2 — Dummy size used when BeginSlot/EndSlot encloses no widget
    ImNodalStyleVar_LinkThickness,          // float (default when Link() gets thickness=0)
    ImNodalStyleVar_GridSize,               // ImVec2
    ImNodalStyleVar_GridSubdivs,            // ImVec2
    ImNodalStyleVar_COUNT,
};
typedef int ImNodalStyleVar;

// =====================================================================
// Behavior flags (bitwise) — same idiom as ImGuiWindowFlags / ImGuiTableFlags
// =====================================================================
// Flags drive binary behaviors on Canvas / Graph / Node / Slot. Anything
// numeric (radius, key, button) stays a regular field on the matching
// *Settings struct. Flags are stored in `*Settings::flags` and are NOT
// reset between frames — pass them every frame just like the other settings.

enum ImNodalCanvasFlags_ {
    ImNodalCanvasFlags_None   = 0,
    ImNodalCanvasFlags_NoGrid = 1 << 0,  // skip the auto grid draw inside BeginCanvas
};
typedef int ImNodalCanvasFlags;

enum ImNodalGraphFlags_ {
    ImNodalGraphFlags_None                  = 0,
    ImNodalGraphFlags_NoBoxSelect           = 1 << 0,  // disable left-drag-from-empty box selection
    ImNodalGraphFlags_NoMultiSelect         = 1 << 1,  // ignore multiSelectKey (single-selection only)
    ImNodalGraphFlags_BoxSelectExcludeNodes = 1 << 2,  // box-select picks links only — nodes are ignored
    ImNodalGraphFlags_BoxSelectExcludeLinks = 1 << 3,  // box-select picks nodes only — links are ignored
    // Box-select selection timing :
    //   default (off)        : selection commits ONCE on mouse release
    //   ImNodalGraphFlags_BoxSelectLive : selection updates every frame as the
    //                                     box grows / shrinks (live preview).
    //                                     The pre-box selection is preserved
    //                                     when the multi-select key is held,
    //                                     cleared otherwise.
    ImNodalGraphFlags_BoxSelectLive         = 1 << 4,
};
typedef int ImNodalGraphFlags;

enum ImNodalNodeFlags_ {
    ImNodalNodeFlags_None             = 0,
    ImNodalNodeFlags_NoBody           = 1 << 0, // skip body fill + border (host paints its own visual)
    ImNodalNodeFlags_HoverHandle      = 1 << 1, // draw a drag bar on top of the node when hovered
    ImNodalNodeFlags_NotMovable       = 1 << 2, // node is selectable but cannot be dragged
    ImNodalNodeFlags_NotSelectable    = 1 << 3, // clicks on the node hit area never enter selection
    ImNodalNodeFlags_HiddenInMinimap  = 1 << 4, // skip the node when painting the minimap
};
typedef int ImNodalNodeFlags;

enum ImNodalSlotFlags_ {
    ImNodalSlotFlags_None              = 0,
    ImNodalSlotFlags_NoConnectionStart = 1 << 0,  // cannot start a link drag from this slot
    ImNodalSlotFlags_NoConnectionEnd   = 1 << 1,  // cannot terminate a link on this slot
    ImNodalSlotFlags_NoContextMenu     = 1 << 2,  // right-click on the slot hit area is ignored
    ImNodalSlotFlags_NoEmptyDummy      = 1 << 3,  // disable the "empty slot -> SlotMinSize Dummy" fallback
};
typedef int ImNodalSlotFlags;

// =====================================================================
// Custom hitbox shape (used by SetNodeHitbox / SetSlotHitbox)
// =====================================================================
// Lets the host override the default rectangular hit area with an arbitrary
// shape (rect / circle / convex polygon). Useful for diamond-shaped nodes,
// reroute dots (small circles), pie slots, etc. The shape is NOT drawn —
// it only drives ImNodal's hit-tests. The host stays responsible for the
// visual, the hitbox simply tells ImNodal where the user can click/hover.
//
// Polygon must be CONVEX. Validity is asserted in SetNodeHitbox /
// SetSlotHitbox at the point of registration (not at hit-test time) so
// the cost is paid once per slot per frame — and only in builds where
// IM_ASSERT is enabled (debug). Concave polygons are rejected by assert.

enum ImNodalHitShape_ {
    ImNodalHitShape_None          = 0,  // no override — fall back to the group rect
    ImNodalHitShape_Rect          = 1,  // axis-aligned rect in screen space
    ImNodalHitShape_Circle        = 2,  // center + radius in screen space
    ImNodalHitShape_ConvexPolygon = 3,  // points in screen space, CCW or CW (convex required)
};
typedef int ImNodalHitShape;

struct ImNodalHitbox {
    ImNodalHitShape type{ImNodalHitShape_None};
    ImRect          rect{};               // ImNodalHitShape_Rect
    ImVec2          center{};             // ImNodalHitShape_Circle
    float           radius{0.0f};         // ImNodalHitShape_Circle
    const ImVec2*   polygonPoints{nullptr};  // ImNodalHitShape_ConvexPolygon — caller-owned, must outlive the call
    int             polygonCount{0};
    ImNodalHitbox() = default;
};

namespace ImNodal {

// User-chosen identity for nodes, slots, links and graphs. Non-zero values
// only — 0 is reserved by ImNodal as the "no such id" sentinel. Hoisted to
// the top of the namespace because SetCurrentEditor (declared below) takes
// a graph id as parameter.
using Id = uintptr_t;

// =====================================================================
// Context
// =====================================================================
// ImNodal mirrors ImGui's context model: ONE Context per application, created
// once at startup and destroyed at shutdown. Multiple editors live as
// multiple `BeginCanvas/EndCanvas` pairs (each identified by its `aId`
// string), and inside each canvas, multiple `BeginGraph/EndGraph` pairs
// (each identified by its `aGraphId`). The per-canvas state (view, pan,
// background interactions) and the per-(canvas, graph) state (selection,
// hover, drag, state machines) are kept in maps inside the Context, keyed
// by the canvas/graph ids the host already passes to Begin*.
//
// Typical lifecycle (host startup):
//   ImNodal::CreateContext();
//   ... main loop ...
//     ImNodal::NewFrame();
//     if (ImNodal::BeginCanvas("editor#1", size, canvasSettings)) {
//       if (ImNodal::BeginGraph(graphId, graphSettings)) { ... ImNodal::EndGraph(); }
//       ImNodal::EndCanvas();
//     }
//   ImNodal::DestroyContext();
//
// Get/SetCurrentContext are an escape hatch for hosts that juggle multiple
// independent Contexts (e.g. plugin systems). Most applications never call
// them — the single Context created at startup stays current for its whole
// lifetime. CanvasState / GraphState entries unseen for more than ~60 frames
// (1s at 60fps) are GC'd automatically inside NewFrame, like ImGui windows.

struct Context;  // opaque

IMNODAL_API Context* CreateContext();
IMNODAL_API void     DestroyContext(Context* apCtx = nullptr);  // null = destroy current
IMNODAL_API Context* GetCurrentContext();
IMNODAL_API void     SetCurrentContext(Context* apCtx);

// Must be called once per frame on the current context, BEFORE any other
// ImNodal call this frame (same contract as ImGui::NewFrame). Clears per-frame
// state: hovered slot/link flags, connection-create transient flags, stale
// drag state left behind by a missing EndConnectionCreate. Also runs the
// GC pass on CanvasState / GraphState. Calling it more than once per frame
// is a no-op.
IMNODAL_API void     NewFrame();

// Address an (canvas, graph) editor by its keys — useful for queries called
// OUTSIDE any Begin/End scope (host code that wants to read selection /
// drag state of a specific editor it isn't currently emitting). Sets the
// current canvas + current graph pointers to the matching entries. The
// entries must exist (i.e. the host must have called Begin/End* for them
// at some point in the recent past, before they were GC'd).
//
// HasEditor returns true if the (canvas, graph) pair has live state — i.e.
// a recent Begin/End cycle exists for it.
IMNODAL_API void     SetCurrentEditor(const char* aCanvasId, Id aGraphId);
IMNODAL_API bool     HasEditor(const char* aCanvasId, Id aGraphId);

// =====================================================================
// Style — ImGui-style theming (Push/Pop colors and vars)
// =====================================================================
// All visual aspects (colors + numeric appearance vars) live in Style.
// Read via GetStyleColorU32 / GetStyleVarFloat / GetStyleVarVec2, override
// temporarily via PushStyleColor / PushStyleVar (matching the ImGui pattern).
// Settings structs (CanvasSettings / NodeSettings / SlotSettings /
// GraphSettings) only carry BEHAVIOR (mouse buttons, keys, modes) — never
// appearance.

struct Style {
    ImU32  Colors[ImNodalCol_COUNT];
    float  NodeRounding;
    float  NodeBorderThickness;
    float  NodeBodyPadding;
    float  NodeHoverHandleHeight;
    ImVec2 SlotMinSize;
    float  LinkThickness;
    ImVec2 GridSize;
    ImVec2 GridSubdivs;

    Style();  // applies sane dark-theme defaults
};

IMNODAL_API Style& GetStyle();
IMNODAL_API ImU32  GetStyleColorU32(ImNodalCol aIdx);
IMNODAL_API float  GetStyleVarFloat(ImNodalStyleVar aIdx);
IMNODAL_API ImVec2 GetStyleVarVec2(ImNodalStyleVar aIdx);

IMNODAL_API bool PushStyleColor(ImNodalCol aIdx, ImU32 aCol);
IMNODAL_API void PopStyleColor(int aCount = 1);

IMNODAL_API bool PushStyleVar(ImNodalStyleVar aIdx, float aVal);
IMNODAL_API bool PushStyleVar(ImNodalStyleVar aIdx, const ImVec2& aVal);
IMNODAL_API void PopStyleVar(int aCount = 1);

// Human-readable names for style enums (useful for editors / debug UI).
IMNODAL_API const char* GetStyleColorName(ImNodalCol aIdx);
IMNODAL_API const char* GetStyleVarName(ImNodalStyleVar aIdx);

// Drop-in style editor widgets — meant to be embedded in the host's settings
// UI (menu, panel, modal). They mutate ImNodal::GetStyle() in place; pair with
// SetCurrentContext if multiple ImNodal contexts coexist.
//   ShowStyleColorsEditor : one ColorEdit4 per ImNodalCol_, with a "Reset to
//   defaults" button at the top.
//   ShowStyleVarsEditor   : one DragFloat / DragFloat2 per ImNodalStyleVar_,
//   with the same reset button.
IMNODAL_API void ShowStyleColorsEditor();
IMNODAL_API void ShowStyleVarsEditor();

// =====================================================================
// Demo window — auto-contained showcase, ImGui::ShowDemoWindow style
// =====================================================================
// Exercises every public feature in a single ImGui window. Pass a bool* if
// you want a closable window. Shares the host's ImNodal context — isolation
// from the host's editor comes from the demo's unique canvas / graph ids,
// not from a private Context. Internal state (positions, sample graph) is
// kept in function-local statics.
IMNODAL_API void ShowDemoWindow(bool* apoOpen = nullptr);

// =====================================================================
// Canvas
// =====================================================================

struct CanvasSettings {
    // Behavior flags (binary toggles) — see ImNodalCanvasFlags_.
    ImNodalCanvasFlags flags{ImNodalCanvasFlags_None};

    // Zoom
    float zoomStep{0.1f};                                      // Amount added/removed per wheel tick
    float zoomMin{0.1f};                                       // Minimum scale
    float zoomMax{10.0f};                                      // Maximum scale
    ImGuiKey resetZoomKey{ImGuiKey_R};                         // Key to reset zoom to 1.0 (ImGuiKey_None to disable)

    // Pan
    ImGuiMouseButton panButton{ImGuiMouseButton_Middle};       // Button used to drag the canvas

    // Context menu
    ImGuiMouseButton contextMenuButton{ImGuiMouseButton_Right};// Button triggering background context menu request

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
// Manual grid draw (only needed if you set ImNodalCanvasFlags_NoGrid).
// Must be called between Begin and End.
// -----------------------------
IMNODAL_API void DrawCanvasGrid();

// =====================================================================
// Graph layer — Graph + Node + Slot primitive + layout containers
// =====================================================================
// Design philosophy : the slot is the primitive. Nodes are draggable /
// selectable containers, but ImNodal does NOT impose any layout — the host
// assembles its node content with BeginLayoutHorizontal / BeginLayoutVertical
// / LayoutSpring / BeginLayoutGroup (see the "Layout primitives" section
// below). A slot can be emitted ANYWHERE : inside a node, inside a plain
// ImGui window, inline with text…
//
// Usage :
//
//     if (BeginCanvas("c", size, canvasSettings)) {
//         if (BeginGraph(graphId, graphSettings)) {
//             if (BeginNode(nodeId, &pos, nodeSettings)) {
//                 BeginLayoutHorizontal("##header");
//                     LayoutSpring();
//                     BeginLayoutGroup(); ImGui::TextUnformatted("Title"); EndLayoutGroup();
//                     LayoutSpring();
//                 EndLayoutHorizontal();
//                 BeginLayoutHorizontal("##body");
//                     if (BeginInputSlot(in_id))  { ImGui::Text("A");   EndSlot(); }
//                     LayoutSpring();
//                     if (BeginOutputSlot(out_id)) { ImGui::Text("Out"); EndSlot(); }
//                 EndLayoutHorizontal();
//                 EndNode();
//             }
//             EndGraph();
//         }
//         EndCanvas();
//     }
//
// BeginGraph must be called INSIDE a BeginCanvas scope.

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
    // Behavior flags (binary toggles) — see ImNodalGraphFlags_.
    ImNodalGraphFlags flags{ImNodalGraphFlags_None};

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
// BeginNode/EndNode opens a draggable, selectable, hit-testable rect at
// the canvas position pointed to by `apPos`. ImNodal paints the body fill
// (ImNodalCol_NodeBody) and the border (NodeBorder / NodeBorderSelected)
// inside EndNode — nothing else. Use the layout primitives BeginLayoutHorizontal /
// BeginLayoutVertical / LayoutSpring / BeginLayoutGroup to assemble the content. Headers, footers, columns, and any tinted band
// are the host's responsibility (paint via GetNodeBackgroundDrawList).
//
// Behaviors are packed in `flags` — see ImNodalNodeFlags_. Combine with
// `|`. Defaults preserve the legacy behavior : movable = ON (set
// NotMovable to disable), no HoverHandle, body drawn, etc. Hosts that
// want a "reroute-like" node combine `ImNodalNodeFlags_NoBody |
// ImNodalNodeFlags_HiddenInMinimap` and call SetNodeHitbox() to override
// the rectangular hit area with a circle.
struct NodeSettings {
    ImNodalNodeFlags flags{ImNodalNodeFlags_None};
    NodeSettings() = default;
};

// aPos is IN/OUT in canvas space. When non-null and movable, dragging updates it.
IMNODAL_API bool BeginNode(Id aNodeId, ImVec2* apPos, const NodeSettings& arSettings = {});
IMNODAL_API void EndNode();

// Set a "color" on the node currently open between BeginNode and EndNode.
// Typical use : the host's accent / header color so the node is recognizable
// in the minimap and other features that key off this color (future).
// Resets to 0 at the next BeginNode — call every frame for the value to persist.
// 0 = no custom color → consumers fall back on their own default.
IMNODAL_API void SetNodeColor(ImU32 aColor);

// =====================================================================
// Layout primitives — Horizontal / Vertical containers + Spring + Group
// =====================================================================
// Generic stack-layout containers usable ONLY inside a BeginNode/EndNode
// scope. Lets the host assemble a node's content with horizontal /
// vertical containers and `LayoutSpring()` distributing the remaining
// space — equivalent in spirit to thedmd's BeginHorizontal/Vertical/Spring
// (but scoped to ImNodal nodes only, no global ImGui pollution).
//
// `aSize.x` and `aSize.y` semantics, per axis :
//     > 0  : forced size in pixels
//     == 0 : natural size (= sum of non-Spring children measured at the
//            previous frame). LayoutSpring() inside is a no-op.
//     < 0  : fill parent. At the top of a node body that means
//            `node.size.x - 2 * NodeBodyPadding` (or .y) measured at the
//            previous frame. First frame falls back to natural.
//
// LayoutSpring(weight) emits a `ImGui::Dummy` of the size needed to reach
// the container's target along its main axis, then `SameLine(0, 0)` for
// horizontal containers. Multi-Spring + weights distribution is supported
// (each Spring takes its proportional share of the gap, sum of weights
// measured the previous frame).
//
// IMPORTANT — bare ImGui widgets do NOT auto-SameLine inside
// BeginLayoutHorizontal. The auto-SameLine machinery is triggered by
// nested BeginLayout* / LayoutSpring / BeginLayoutGroup calls only. Wrap
// any bare widget in BeginLayoutGroup/EndLayoutGroup (see below) so it
// counts as a layout child.
//
// Typical node layout :
//
//     ImNodal::BeginLayoutVertical("##node", ImVec2(-1.0f, 0.0f));
//         // header centered on the node width
//         ImNodal::BeginLayoutHorizontal("##header");
//             ImNodal::LayoutSpring();
//             ImNodal::BeginLayoutGroup();
//                 ImGui::TextUnformatted("Node title");
//             ImNodal::EndLayoutGroup();
//             ImNodal::LayoutSpring();
//         ImNodal::EndLayoutHorizontal();
//
//         // body : inputs left, outputs right, Spring between
//         ImNodal::BeginLayoutHorizontal("##body");
//             for (auto& s : inputs) draw_slot(s);
//             ImNodal::LayoutSpring();
//             for (auto& s : outputs) draw_slot(s);
//         ImNodal::EndLayoutHorizontal();
//     ImNodal::EndLayoutVertical();
//
// EndNode asserts the layout stack is empty (host must close every
// container it opened). NewFrame clears any leftover state from a missing
// EndLayoutHorizontal/EndLayoutVertical the previous frame.
IMNODAL_API bool BeginLayoutHorizontal(const char* aId, const ImVec2& aSize = ImVec2(-1.0f, 0.0f));
IMNODAL_API void EndLayoutHorizontal();
IMNODAL_API bool BeginLayoutVertical(const char* aId, const ImVec2& aSize = ImVec2(0.0f, -1.0f));
IMNODAL_API void EndLayoutVertical();
IMNODAL_API void LayoutSpring(float aWeight = 1.0f);

// Wrap any bare ImGui widget(s) so they count as a single child of the
// enclosing BeginLayoutHorizontal / BeginLayoutVertical. Without this,
// `ImGui::Dummy(...)`, `ImGui::TextUnformatted(...)`, `ImGui::Button(...)`
// etc. don't trigger the auto-SameLine that BeginLayout* uses to chain
// children — the bare widget falls on the next line.
//
// Inside : the host emits any ImGui calls. The whole content is wrapped
// in `ImGui::BeginGroup`/`ImGui::EndGroup` so it's treated as a single
// layout unit with a measurable rect (useful when the host wants to query
// `ImGui::GetItemRectMin/Max` after `EndLayoutGroup`).
//
// No-op outside of a BeginLayout* scope — safe to use unconditionally.
//
// Example :
//
//     BeginLayoutHorizontal("row");
//         BeginLayoutGroup();   ImGui::Dummy(ImVec2(12,12));      EndLayoutGroup();
//         BeginLayoutGroup();   ImGui::TextUnformatted("label");  EndLayoutGroup();
//         LayoutSpring(1.0f);
//     EndLayoutHorizontal();
IMNODAL_API bool BeginLayoutGroup();
IMNODAL_API void EndLayoutGroup();

// -----------------------------
// Slot (primitive — usable anywhere)
// -----------------------------
// BeginSlot/EndSlot is a CAPTURE-ONLY scope, in the spirit of
// thedmd/imgui-node-editor's BeginPin/EndPin. ImNodal does NOT render
// anything inside it — no dot, no label, no padding. The host emits its
// own widgets (Text, Icon, Dummy, custom drawing, ...) between Begin and
// End; ImNodal only :
//   - opens an ImGui group around the host content,
//   - computes the link pivot from the resulting group rect,
//   - hit-tests for hover / click / right-click,
//   - drives the link-drag state machine.
//
// To draw the visible dot (or any other slot mark), call GetSlotScreenPos
// AFTER EndSlot and paint at that point yourself :
//
//     if (ImNodal::BeginInputSlot(slotId)) {
//         ImGui::Dummy(ImVec2(12, 12));               // reserve room for the mark
//         ImGui::SameLine(0, 4);
//         ImGui::TextUnformatted("label");
//         ImNodal::EndSlot();
//
//         // Paint the dot at the link endpoint :
//         const ImVec2 c = ImNodal::GetSlotScreenPos(slotId);
//         const ImU32 col = ImNodal::IsSlotHovered(slotId) ? hoverCol
//                         : ImNodal::IsSlotConnected(slotId) ? connCol
//                         : restCol;
//         ImGui::GetWindowDrawList()->AddCircleFilled(c, 5.0f, col);
//     }
//
// Default link-pivot placement (the point where links connect to the slot)
// is the CENTER of the group rect, regardless of role — same convention
// as thedmd/imgui-node-editor. The role only drives the link tangent
// (Input → -X, Output → +X, InOut → auto).
//
// If your visible mark sits on an edge of the content (typical : icon at
// the right of an output, icon at the left of an input), call
// SlotAlignment(ImVec2(edgeX, 0.5f)) between BeginSlot and EndSlot to
// pull the pivot to that edge — see "Slot pivot control" below.
//
// Empty slot : if the host emits NOTHING between BeginSlot and EndSlot,
// ImNodal substitutes a Dummy of `Style.SlotMinSize` so the slot still has
// a hit area. The host can then draw its own mark at GetSlotScreenPos —
// the "minimal button" pattern.
struct SlotSettings {
    uint32_t typeTag{0};                              // user-chosen type tag (for M2 connection rules)
    ImNodalSlotFlags flags{ImNodalSlotFlags_None};    // see ImNodalSlotFlags_
    SlotSettings() = default;
};

IMNODAL_API bool BeginSlot(Id aSlotId, SlotRole aRole, const SlotSettings& arSettings = {});
IMNODAL_API void EndSlot();
// Convenience wrappers
IMNODAL_API bool BeginInputSlot (Id aSlotId, const SlotSettings& arSettings = {});
IMNODAL_API bool BeginOutputSlot(Id aSlotId, const SlotSettings& arSettings = {});

// -----------------------------
// Graph queries & interactions
// -----------------------------
IMNODAL_API ImVec2 GetSlotScreenPos(Id aSlotId);
IMNODAL_API ImVec2 GetSlotTangent(Id aSlotId);         // unit vector pointing away from the node edge
IMNODAL_API ImRect GetSlotHitRect(Id aSlotId);         // last frame's screen-space hit rect (= group rect of the slot)
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

// =====================================================================
// Custom links — low-level primitives (drawing + hit-test combined)
// =====================================================================
// Workflow:
//   if (ImNodal::BeginLink(linkId, fromSlot, toSlot, thickness, color)) {
//       // any composition of:
//       ImNodal::LinkBezierSegment(p0, p1, fromTan, toTan, segments);
//       ImNodal::LinkLineSegment(p0, p1);
//       ImNodal::LinkPolyline(points, count);
//       ImNodal::EndLink();
//   }
// All coordinates are in SCREEN-SPACE (cf. GetSlotScreenPos).
// Primitives accumulate a polyline that is drawn in a single pass by EndLink
// with the final color resolved (base / hovered / selected). The hit-test of
// a segment covers an OBB of width max(thickness*2, 6/canvasScale).

IMNODAL_API bool   BeginLink(Id aLinkId, Id aFromSlotId, Id aToSlotId, float aThickness = 0.0f, ImU32 aColor = 0);
IMNODAL_API void   EndLink();

// Geometry of the current link, pre-resolved by BeginLink (handles the
// InOut (0,0) sentinel -> dynamic horizontal tangent). Returns (0,0) outside
// of a BeginLink/EndLink scope.
IMNODAL_API ImVec2 GetLinkFromPos();
IMNODAL_API ImVec2 GetLinkToPos();
IMNODAL_API ImVec2 GetLinkFromTangent();   // unit vector pointing AWAY from from-slot
IMNODAL_API ImVec2 GetLinkToTangent();     // unit vector pointing AWAY from to-slot

// Primitive: a straight line. Hit-test = OBB of width
// max(thickness*2, 6/canvasScale) along the segment.
IMNODAL_API void   LinkLineSegment(const ImVec2& aP0, const ImVec2& aP1);

// Primitive: cubic Bezier sampled into N segments, calls LinkLineSegment N
// times. Tangents = unit vectors pointing AWAY from the endpoints.
// aSegments=0 -> 24. Control point distance = clamp(0.4*chord, 30, 200).
IMNODAL_API void   LinkBezierSegment(const ImVec2& aP0, const ImVec2& aP1, const ImVec2& aFromTangent, const ImVec2& aToTangent, int aSegments = 0);

// Primitive: pre-computed polyline (Manhattan, orthogonal, custom shape).
// Emits (aCount-1) calls to LinkLineSegment.
IMNODAL_API void   LinkPolyline(const ImVec2* apPoints, int aCount);

// Optional override of the hit-test threshold for the current link. Call
// between BeginLink and the first primitive.
IMNODAL_API void   SetLinkHitThickness(float aPixelThreshold);

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
//         } else if (QueryNewNodeFromSlot(&from)) {
//             // user dropped on empty canvas — typically open a "create node
//             // connected to `from`" popup, then on next frame call AcceptNewNodeFromSlot
//             if (AcceptNewNodeFromSlot()) { open_node_creation_popup(from); }
//         }
//         EndConnectionCreate();
//     }
//
// BeginConnectionCreate returns true as soon as the user starts dragging from
// a slot. QueryNewLink returns true every frame while a compatible target slot
// is hovered. QueryNewNodeFromSlot returns true while the drag is on empty
// canvas (no slot under cursor). AcceptNewLink/AcceptNewNodeFromSlot light the
// preview green and return true once on the frame the mouse is released
// (commit). RejectNewLink paints it red.

IMNODAL_API bool BeginConnectionCreate();
// Slot id the user is currently dragging a link from (0 if no drag active).
// Useful to resolve the source slot's host-side color BEFORE calling
// BeginConnectionCreate so the preview wire's idle tint matches.
IMNODAL_API Id GetDraggingFromSlot();
IMNODAL_API bool QueryNewLink(Id* apoFromSlotId, Id* apoToSlotId);
IMNODAL_API bool QueryNewNodeFromSlot(Id* apoFromSlotId);
IMNODAL_API bool AcceptNewLink(ImU32 aColor = 0);
IMNODAL_API bool AcceptNewNodeFromSlot(ImU32 aColor = 0);
IMNODAL_API void RejectNewLink(const char* aReason = nullptr);
IMNODAL_API void EndConnectionCreate();

// =====================================================================
// Multi-selection
// =====================================================================
// Selection is a SET of node ids and a SET of link ids per graph. Shift +
// left-click on a node/link toggles its membership in the set. The legacy
// single-id Get/SetSelectedNode/Link helpers still work — they return the
// "first" element / replace the whole set respectively.
IMNODAL_API int  GetSelectedObjectCount();
IMNODAL_API int  GetSelectedNodes(Id* apoBuffer, int aCapacity);   // returns count written
IMNODAL_API int  GetSelectedLinks(Id* apoBuffer, int aCapacity);
IMNODAL_API bool HasSelectionChanged();                            // true on the frame the selection changed
IMNODAL_API void AddToSelection(Id aId);                           // node OR link id; ImNodal looks it up
IMNODAL_API void RemoveFromSelection(Id aId);
IMNODAL_API void ClearSelection();

// =====================================================================
// Direct hover queries
// =====================================================================
IMNODAL_API Id GetHoveredSlot();   // 0 if none
IMNODAL_API Id GetHoveredNode();   // 0 if none
IMNODAL_API Id GetHoveredLink();   // 0 if none

// =====================================================================
// Context-menu requests (right-click on a specific item)
// =====================================================================
// Each returns true on the frame the user right-clicked the corresponding
// item. The caller typically opens an ImGui popup using the returned id.
IMNODAL_API bool IsNodeContextMenuRequested(Id* apoNodeId);
IMNODAL_API bool IsSlotContextMenuRequested(Id* apoSlotId);
IMNODAL_API bool IsLinkContextMenuRequested(Id* apoLinkId);

// =====================================================================
// Delete state machine
// =====================================================================
// Triggered by the Delete key (deletes everything currently selected). Mirrors
// thedmd's BeginDelete API. Use Accept/Reject once per Query result; the
// caller is responsible for actually removing the entity from its data and
// will simply stop emitting it on the next frame.
//
//     if (BeginDelete()) {
//         Id linkId;
//         while (QueryDeletedLink(&linkId)) {
//             if (allow_link_delete(linkId)) AcceptDelete();
//             else                            RejectDelete();
//         }
//         Id nodeId;
//         while (QueryDeletedNode(&nodeId)) {
//             if (allow_node_delete(nodeId)) AcceptDelete();
//             else                            RejectDelete();
//         }
//         EndDelete();
//     }
IMNODAL_API bool BeginDelete();
IMNODAL_API bool QueryDeletedLink(Id* apoLinkId);
IMNODAL_API bool QueryDeletedNode(Id* apoNodeId);
IMNODAL_API bool AcceptDelete();
IMNODAL_API void RejectDelete();
IMNODAL_API void EndDelete();

// =====================================================================
// Shortcut machine (Ctrl+C/V/X/D/A) — call between BeginGraph/EndGraph
// =====================================================================
// AcceptXxx returns true exactly on the frame the corresponding shortcut
// fired. Use GetActionContextNodes/Links to know which entities are in scope
// for the shortcut — ImNodal uses the current selection at trigger time.
//
//     if (BeginShortcut()) {
//         if      (AcceptCopy())      do_copy();
//         else if (AcceptPaste())     do_paste();
//         else if (AcceptCut())       do_cut();
//         else if (AcceptDuplicate()) do_duplicate();
//         else if (AcceptSelectAll()) do_select_all();
//         EndShortcut();
//     }
IMNODAL_API bool BeginShortcut();
IMNODAL_API bool AcceptCopy();
IMNODAL_API bool AcceptPaste();
IMNODAL_API bool AcceptCut();
IMNODAL_API bool AcceptDuplicate();
IMNODAL_API bool AcceptSelectAll();
IMNODAL_API void EndShortcut();
IMNODAL_API int  GetActionContextNodes(Id* apoBuffer, int aCapacity);
IMNODAL_API int  GetActionContextLinks(Id* apoBuffer, int aCapacity);

// =====================================================================
// Flow animation on a link
// =====================================================================
// Call right AFTER Link(...) for the same id. Draws moving dots along the
// curve. Speed is in canvas units per second. aColor==0 picks a default
// accent based on the link color.
IMNODAL_API void FlowLink(Id aLinkId, float aSpeed = 2.0f, ImU32 aColor = 0);

// =====================================================================
// MiniMap (overview window anchored to a corner of the canvas)
// =====================================================================
// Draws a scaled-down view of the current graph: each node as a rectangle
// (using NodeSettings::miniMapColor or NodeBody color as fallback) plus a
// frame outlining the visible canvas area. Call between BeginGraph and
// EndGraph (after the nodes have been emitted so their last-frame screen
// rects are up to date). Acts like a floating window above the graph: while
// the cursor is over the minimap, ALL graph interactions (node hover/drag,
// link click, slot connection, pan, box-select, bg context-menu) are
// blocked. Click-or-drag inside the minimap recenters the canvas on the
// pointed point (UE-style). Wheel zooms the canvas.

struct MiniMapSettings {
    ImVec2        size{180.0f, 120.0f};      // pixel size of the minimap rect
    ImNodalCorner anchor{ImNodalCorner_TopRight};
    ImVec2        offset{10.0f, 10.0f};       // px from the anchored corner
    // Visual overrides — 0 means "use the matching Style color".
    ImU32         bgColor{0};                 // ImNodalCol_MiniMapBg
    ImU32         borderColor{0};             // ImNodalCol_MiniMapBorder
    float         borderThickness{1.0f};
    ImU32         viewportRectColor{0};       // ImNodalCol_MiniMapViewport
    float         viewportRectThickness{1.5f};
    MiniMapSettings() = default;
};

IMNODAL_API void ShowMiniMap(const MiniMapSettings& arSettings = {});

// =====================================================================
// Per-node draw lists (custom overlays under or above node content)
// =====================================================================
// Returns the canvas's draw list with the active channel switched to the
// matching z-level. Call between BeginNode and EndNode, draw your shapes
// IMMEDIATELY, and do NOT emit ImGui widgets afterwards until the next
// ImNodal call (which will restore the channel). Typical use: a colored
// header band, a debug highlight, a progress bar overlay.
IMNODAL_API ImDrawList* GetNodeBackgroundDrawList(Id aNodeId);   // under content, above node body
IMNODAL_API ImDrawList* GetNodeForegroundDrawList(Id aNodeId);   // above content, below overlay
IMNODAL_API ImRect      GetNodeRect(Id aNodeId);                 // last-frame screen-space rect

// =====================================================================
// Slot pivot control — same model as thedmd/imgui-node-editor's
// PinPivotAlignment / PinPivotSize.
// =====================================================================
// The slot pivot is a RECT inside the slot's group rect. The link endpoint
// snaps to that rect (point-mode when SlotSize is zero). Call between
// BeginSlot and EndSlot to override per-slot ; the override resets at
// EndSlot to the defaults below.
//
//   - SlotAlignment(a) : 2D alignment of pivot.Min within the group rect.
//                        (0,0) = group top-left, (1,1) = group bottom-right.
//                        DEFAULT (0.5, 0.5) — the link endpoint sits at
//                        the CENTER of whatever the host emitted between
//                        BeginSlot and EndSlot, regardless of role. The
//                        role only drives the link tangent.
//   - SlotSize(s)      : pixel size of the pivot rect (added below/right
//                        of pivot.Min). DEFAULT (0,0) → pivot is a point.
//
// Typical override : a slot whose visible mark is on the right edge of
// the content (output with icon-after-label) :
//
//     if (BeginOutputSlot(id)) {
//         SlotAlignment(ImVec2(1.0f, 0.5f));   // pivot at right edge
//         SlotSize(ImVec2(0, 0));              // pivot is a point
//         ImGui::TextUnformatted("name");
//         ImGui::SameLine();
//         ImGui::Dummy(ImVec2(12, 12));        // <- where you'll paint the dot
//         EndSlot();
//     }
//     // then paint the dot at GetSlotScreenPos(id).
IMNODAL_API void SlotAlignment(const ImVec2& aAlignment);
IMNODAL_API void SlotSize(const ImVec2& aSizePx);
// Pixel offset added to the computed screenPos AFTER alignment + size are
// applied. Handy when you want the link endpoint to land a few pixels off
// the natural edge — e.g. shift the pivot by `-radius` so a host-painted
// dot at GetSlotScreenPos sits ENTIRELY inside the group instead of being
// half-cut by the edge. Resets to (0, 0) at EndSlot like the other two.
IMNODAL_API void SlotPivotOffset(const ImVec2& aOffsetPx);

// =====================================================================
// Custom hitbox — override the rectangular hit area of a node or slot
// =====================================================================
// By default a node hovers when the mouse is inside its rect, and a slot
// hovers when the mouse is inside its group rect. Hosts that draw odd
// shapes (a circular reroute dot, a diamond decision node, a triangular
// flow gate, ...) need the hit area to follow the visual instead of the
// AABB. Call SetNodeHitbox / SetSlotHitbox between Begin* and End* to
// supply a custom shape — ImNodal switches its hover/click test to that
// shape for the rest of the frame.
//
// Coordinates are in SCREEN space. The shape is consumed at End*; it
// resets to "no override" the next frame, just like SlotAlignment / Push*.
//
// Example — circular reroute node :
//
//     NodeSettings ns;
//     ns.flags = ImNodalNodeFlags_NoBody | ImNodalNodeFlags_HiddenInMinimap;
//     if (ImNodal::BeginNode(nodeId, &pos, ns)) {
//         ImNodal::BeginSlot(slotId, ImNodal::SlotRole_InOut);
//         ImGui::Dummy(ImVec2(2*r, 2*r));
//         const ImVec2 c = ImGui::GetItemRectMin() + ImVec2(r, r);
//         ImNodalHitbox hit;
//         hit.type = ImNodalHitShape_Circle;
//         hit.center = c;
//         hit.radius = r + 6.0f;
//         ImNodal::SetSlotHitbox(hit);
//         ImNodal::EndSlot();
//         ImNodal::SetNodeHitbox(hit);
//         ImNodal::EndNode();
//     }
//     // Host paints the visible dot at `c` afterwards.
//
// Example — diamond decision node with one slot per corner :
//     // Inside BeginNode, draw the diamond yourself, position 4 slots at
//     // the corners using BeginLayoutHorizontal/Vertical or absolute Dummy()s, and pass each
//     // slot's hitbox as a small circle around its corner. Pass a 4-point
//     // ConvexPolygon to SetNodeHitbox to make the whole diamond clickable.
//
// Convex polygons only — ConvexPolygon hitboxes are validated at the
// SetSlotHitbox / SetNodeHitbox call via IM_ASSERT (debug only). Concave
// polygons must be split by the host or replaced with a rect/circle.
IMNODAL_API void SetSlotHitbox(const ImNodalHitbox& aHitbox);
IMNODAL_API void SetNodeHitbox(const ImNodalHitbox& aHitbox);

// =====================================================================
// Navigation helpers
// =====================================================================
// Recenter (and optionally zoom) the canvas on the bounding box of all
// nodes, or only the current selection. Call inside BeginCanvas scope.
IMNODAL_API void NavigateToContent(bool aZoomToFit = true, float aMarginRatio = 0.1f);
IMNODAL_API void NavigateToSelection(bool aZoomToFit = true, float aMarginRatio = 0.25f);

}  // namespace ImNodal
