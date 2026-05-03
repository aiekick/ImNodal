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
// Style — ImGui-style theming (Push/Pop colors and vars)
// =====================================================================
// All visual aspects (colors + numeric appearance vars) live in Style.
// Read via GetStyleColorU32 / GetStyleVarFloat / GetStyleVarVec2, override
// temporarily via PushStyleColor / PushStyleVar (matching the ImGui pattern).
// Settings structs (CanvasSettings / NodeSettings / SlotSettings /
// GraphSettings) only carry BEHAVIOR (mouse buttons, keys, modes) — never
// appearance.

enum ImNodalCol_ {
    ImNodalCol_GridLine = 0,
    ImNodalCol_GridSubLine,
    ImNodalCol_NodeBody,
    ImNodalCol_NodeHeader,                 // default header tint when host doesn't push a per-node color
    ImNodalCol_NodeBorder,
    ImNodalCol_NodeBorderSelected,
    ImNodalCol_NodeHoverHandle,
    // Slot-dot colors are RECOMMENDED defaults that hosts may read when
    // drawing their own dot. ImNodal itself does NOT render the dot inside
    // BeginSlot/EndSlot — only the reroute primitive uses these values.
    ImNodalCol_SlotDot,
    ImNodalCol_SlotDotConnected,
    ImNodalCol_SlotDotHovered,
    ImNodalCol_SlotHoverFill,              // interaction halo painted by EndGraph behind the slot rect
    ImNodalCol_SlotHoverBorder,
    ImNodalCol_Link,                       // default link color (when host passes 0)
    ImNodalCol_LinkHovered,
    ImNodalCol_LinkSelected,
    ImNodalCol_RerouteBorder,              // faint frame at rest
    ImNodalCol_RerouteBorderSelected,
    ImNodalCol_BoxSelectFill,
    ImNodalCol_BoxSelectBorder,
    ImNodalCol_LinkPreviewIdle,            // wire color while dragging, no target hovered
    ImNodalCol_LinkPreviewAccept,          // wire color when AcceptNewLink fired
    ImNodalCol_LinkPreviewReject,          // wire color when RejectNewLink fired
    ImNodalCol_FlowDot,                    // moving dot color along FlowLink
    ImNodalCol_COUNT,
};
typedef int ImNodalCol;

enum ImNodalStyleVar_ {
    ImNodalStyleVar_NodeRounding = 0,      // float
    ImNodalStyleVar_NodeBorderThickness,   // float
    ImNodalStyleVar_NodeHeaderPadding,     // float
    ImNodalStyleVar_NodeBodyPadding,       // float
    ImNodalStyleVar_NodeColumnSpacing,     // float
    ImNodalStyleVar_NodeHoverHandleHeight, // float
    ImNodalStyleVar_SlotDotRadius,         // float — recommended dot radius for hosts; not drawn by ImNodal
    ImNodalStyleVar_SlotMinSize,           // ImVec2 — Dummy size used when BeginSlot/EndSlot encloses no widget
    ImNodalStyleVar_LinkThickness,         // float (default when Link() gets thickness=0)
    ImNodalStyleVar_GridSize,              // ImVec2
    ImNodalStyleVar_GridSubdivs,           // ImVec2
    ImNodalStyleVar_COUNT,
};
typedef int ImNodalStyleVar;

struct Style {
    ImU32  Colors[ImNodalCol_COUNT];
    float  NodeRounding;
    float  NodeBorderThickness;
    float  NodeHeaderPadding;
    float  NodeBodyPadding;
    float  NodeColumnSpacing;
    float  NodeHoverHandleHeight;
    float  SlotDotRadius;
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

IMNODAL_API void PushStyleColor(ImNodalCol aIdx, ImU32 aCol);
IMNODAL_API void PopStyleColor(int aCount = 1);

IMNODAL_API void PushStyleVar(ImNodalStyleVar aIdx, float aVal);
IMNODAL_API void PushStyleVar(ImNodalStyleVar aIdx, const ImVec2& aVal);
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
// you want a closable window. The demo uses the CURRENT ImNodal context;
// create one before calling. Internal state (nodes / links / positions) is
// kept in function-local statics — it survives between frames but is not
// shared with the caller's graph.
IMNODAL_API void ShowDemoWindow(bool* apoOpen = nullptr);

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
//                     if (BeginInputSlot(slotId)) { ImGui::Text("A"); EndSlot(); }
//                     EndInputs();
//                 }
//                 if (BeginCenter())  { /* body widget */ EndCenter(); }
//                 if (BeginOutputs()) {
//                     if (BeginOutputSlot(slotId2)) { ImGui::Text("Out"); EndSlot(); }
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
// Header tint: by default, BeginNode reads ImNodalCol_NodeHeader from the
// current style. To customize per-node, push the desired color before
// BeginNode :
//     ImNodal::PushStyleColor(ImNodalCol_NodeHeader, datas.color);
//     ImNodal::BeginNode(id, &pos);
//     ...
//     ImNodal::EndNode();
//     ImNodal::PopStyleColor();
struct NodeSettings {
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

// Layout container that horizontally aligns whatever ImGui widgets are
// emitted between Begin and End. Use it to center a title in a header, push
// a label to the right of the footer, or align any group of widgets within
// a row. The widgets are real ImGui widgets — full styling (PushStyleColor,
// fonts, hovering, clicking, custom draw) works as usual.
//
//   ratio: 0.0 = left, 0.5 = center, 1.0 = right (along the X axis)
//   availableWidth:
//     - > 0 : explicit width to align within
//     - 0   : auto. Inside a node → use the node's width (from the previous
//             frame's EndNode); outside → ImGui::GetContentRegionAvail().x
//
// Implementation note: alignment uses the group's width MEASURED on the
// PREVIOUS frame to compute this frame's indent. The first frame a given
// scope is laid out, the contents are left-aligned (no measurement yet);
// from the second frame on, alignment is correct. Width changes between
// frames produce a 1-frame visual lag — fine for most node layouts, since
// the node size itself follows the same lag.
IMNODAL_API void BeginAlign(float aRatio, float aAvailableWidth = 0.0f);
IMNODAL_API void EndAlign();

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
    uint32_t typeTag{0};   // user-chosen type tag (for M2 connection rules)
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
IMNODAL_API void FlowLink(Id aLinkId, float aSpeed = 200.0f, ImU32 aColor = 0);

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
// Navigation helpers
// =====================================================================
// Recenter (and optionally zoom) the canvas on the bounding box of all
// nodes, or only the current selection. Call inside BeginCanvas scope.
IMNODAL_API void NavigateToContent(bool aZoomToFit = true, float aMarginRatio = 0.1f);
IMNODAL_API void NavigateToSelection(bool aZoomToFit = true, float aMarginRatio = 0.25f);

// =====================================================================
// Reroute node (M2)
// =====================================================================
// A minimal pass-through node : no header / body / footer, just one InOut
// slot at the node center. Used to bend links. Still draggable, clicking +
// dragging from the slot starts a new connection.
//
// Like BeginSlot/EndSlot, BeginRerouteNode/EndRerouteNode is CAPTURE-ONLY.
// ImNodal sets up the node + slot, sizes the hit area to a circle of
// `aHitRadius` around the slot pivot, and shows a hover-only drag handle.
// The host renders the visible dot (and any selection ring it wants) AFTER
// EndRerouteNode using GetSlotScreenPos / IsNodeSelected / IsSlotHovered.
//
//     ImNodal::BeginRerouteNode(nodeId, slotId, &pos);
//     ImNodal::EndRerouteNode();
//     const ImVec2 c = ImNodal::GetSlotScreenPos(slotId);
//     auto* dl = ImGui::GetWindowDrawList();
//     dl->AddCircleFilled(c, 5.0f, dotColor);
//     if (ImNodal::IsNodeSelected(nodeId))
//         dl->AddCircle(c, 11.0f, selBorderColor, 0, 1.5f);
//
// `aHitRadius` controls the circular hit zone of the reroute slot — keep
// it close to the radius your host draws so the visible dot and the
// clickable area match.
IMNODAL_API bool BeginRerouteNode(Id aNodeId, Id aSlotId, ImVec2* apPos,
                                  const NodeSettings& arSettings = {},
                                  float aHitRadius = 5.0f);
IMNODAL_API void EndRerouteNode();

}  // namespace ImNodal
