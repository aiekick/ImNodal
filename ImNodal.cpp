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

// Local-space transform approach is adapted from thedmd/imgui-node-editor's
// ImCanvas (https://github.com/thedmd/imgui-node-editor). Stripped, simplified,
// and reshaped to match ImNodal's style.

#include "ImNodal.h"

#include <cmath>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ImNodal {

// =====================================================================
// Graph layer — internal types
// =====================================================================

struct SlotState {
    Id        parentNode{0};    // 0 = standalone
    Id        graphId{0};       // 0 = standalone (outside any BeginGraph)
    SlotRole  role{SlotRole_Input};
    uint32_t  typeTag{0};
    ImVec2    screenPos{};      // dot center, committed at EndSlot
    ImVec2    tangent{};        // unit vector pointing away from the slot side
    float     dotRadius{5.0f};
    // Dot color snapshot taken from SlotSettings at BeginSlot time.
    ImU32     dotColor{IM_COL32(200, 200, 200, 255)};
    ImU32     dotColorConnected{IM_COL32(255, 220, 0, 255)};
    ImU32     dotColorHovered{IM_COL32(255, 255, 255, 255)};
    // Hit rect in local-space, captured at EndSlot. Used by the next frame's
    // BeginSlot to set rSlot.hovered EARLY (so IsSlotHovered() works between
    // BeginSlot and EndSlot — typically where the host queries double-clicks)
    // and by EndGraph to draw a hover frame around the slot.
    ImRect    lastHitRect{};
    bool      hovered{false};
    bool      ctxMenuRequested{false}; // set by EndSlot when right-clicked
    bool      connected{false}; // set by M2
};

struct NodeState {
    ImVec2   pos{};           // canvas-space top-left
    ImVec2   size{};          // filled at EndNode
    Id       graphId{0};
    bool     hovered{false};
    bool     selected{false};
    bool     dragging{false};
    bool     ctxMenuRequested{false};  // right-click on node, set by EndNode
    // Settings snapshot for EndNode draw
    NodeSettings settings;
    // Header rect (screen), filled at EndHeader, used by EndNode for header tint
    ImRect   headerScreenRect{};
    // Last frame's screen-space rect (for GetNodeRect / per-node drawlists)
    ImRect   lastScreenRect{};
    bool     hasHeader{false};
    // Body column tracking (to emit SameLine between Inputs/Center/Outputs automatically)
    bool     bodyColumnOpened{false};
    // Reroute marker: set by BeginRerouteNode. Switches the selection frame
    // and the slot hover halo from rectangle to CIRCLE — matches UE-style
    // reroutes (a small dot with a ring on hover/select instead of a square).
    bool     isReroute{false};
    Id       rerouteSlotId{0};   // valid only when isReroute (used to find dot center)
    // Pointer to user's master copy of pos — updated on drag at EndNode
    ImVec2*  userPosPtr{nullptr};
};

// Per-scope persistent state for BeginAlign/EndAlign. The previous frame's
// measured group width drives this frame's indent.
struct AlignSlot {
    float lastWidth{0.0f};      // group width measured at last EndAlign
    float appliedIndent{0.0f};  // indent applied at BeginAlign, undone at EndAlign
};

struct LinkState {
    Id    fromSlot{0};
    Id    toSlot{0};
    Id    graphId{0};
    ImU32 color{0};
    float thickness{3.0f};
    // Bezier sampled control points cached by Link() for FlowLink() to reuse.
    ImVec2 cachedFromPos{};
    ImVec2 cachedToPos{};
    ImVec2 cachedP1{};
    ImVec2 cachedP2{};
    bool   bezierCached{false};
    // Per-frame interaction state (computed by Link(), reset at NewFrame).
    bool  hovered{false};
    bool  clicked{false};
    bool  doubleClicked{false};
    bool  selected{false};
    bool  ctxMenuRequested{false};
};

struct GraphState {
    Id       id{0};
    GraphSettings settings{};
    // Splitter channels managed inside BeginGraph/EndGraph
    bool     splitterActive{false};
    int      preBeginChannel{0};
    // Multi-selection: a SET of node ids and link ids. Last-clicked id is
    // tracked so the legacy GetSelectedNode/Link can return a stable "first"
    // element (the one the user touched most recently).
    std::unordered_set<Id> selectedNodes;
    std::unordered_set<Id> selectedLinks;
    Id       lastSelectedNode{0};
    Id       lastSelectedLink{0};
    // Snapshot of the previous frame's selection — used to fire HasSelectionChanged.
    std::unordered_set<Id> prevSelectedNodes;
    std::unordered_set<Id> prevSelectedLinks;
    bool     selectionChangedThisFrame{false};
    // Draw order / node Z (M1: simple insertion order)
    std::vector<Id> frameNodeOrder;
    // Slots emitted this frame INSIDE this graph. Their dots are drawn at
    // EndGraph (after Link() has had a chance to set rSlot.connected) so the
    // accent color reflects the up-to-date connection state.
    std::vector<Id> frameSlotOrder;
    // Set by EndNode or Link() when they consumed the left click this frame.
    // EndGraph reads this to decide whether a left click on empty space should
    // clear selection.
    bool     clickConsumedThisFrame{false};
};

// =====================================================================
// Context (opaque publicly — declared in ImNodal.h)
// =====================================================================

struct Context {
    bool active{false};

    CanvasSettings settings;

    // Widget geometry (screen space)
    ImVec2 widgetPos{};
    ImVec2 widgetSize{};
    ImRect widgetRect{};

    // View: screen = canvas * scale + origin + widgetPos
    ImVec2 origin{};              // Translation in screen pixels, relative to widgetPos
    float  scale{1.0f};
    float  invScale{1.0f};
    ImVec2 viewTransformPos{};    // origin + widgetPos — recomputed on change

    // Visible canvas-space rect (valid while inside local space)
    ImRect viewRect{};

    // Draw list state snapshot (for local-space vertex transform)
    ImDrawList* drawList{nullptr};
    int         expectedChannel{0};
    int         cmdBufferSize{0};
    int         startVertexIdx{0};
    float       lastFringeScale{1.0f};

    // Pan
    bool   isPanning{false};
    ImVec2 panStartOrigin{};

    // Input backup (for local-space mouse coord transform)
    ImVec2 mousePosBackup{};
    ImVec2 mousePosPrevBackup{};
    ImVec2 mouseClickedPosBackup[IM_ARRAYSIZE(ImGuiIO::MouseClickedPos)]{};

    // Viewport backup
    ImVec2 windowPosBackup{};
    ImVec2 viewportPosBackup{};
    ImVec2 viewportSizeBackup{};
    ImVec2 viewportWorkPosBackup{};
    ImVec2 viewportWorkSizeBackup{};

    // Layout backup
    ImVec2 windowCursorMaxBackup{};

    // Suspend nesting
    int suspendCounter{0};

    // Background interaction flags (computed in EndCanvas, queryable after)
    bool hovered{false};
    bool bgClicked{false};
    bool bgDoubleClicked{false};
    bool bgCtxMenuRequested{false};

    // Persisted across frames to know when to auto-center
    bool viewInitialized{false};

    // -----------------------------
    // Graph layer state
    // -----------------------------
    std::unordered_map<Id, NodeState> nodes;
    std::unordered_map<Id, SlotState> slots;
    std::unordered_map<Id, LinkState> links;
    std::unordered_map<Id, GraphState> graphs;

    // Currently open graph (0 = none)
    Id       currentGraphId{0};
    bool     graphActive{false};

    // Currently open node (0 = none)
    Id       currentNodeId{0};

    // Currently open slot (0 = none)
    Id       currentSlotId{0};

    // Section tracking (so EndFooter etc. know what to close)
    enum Section { Section_None, Section_Header, Section_Inputs, Section_Center, Section_Outputs, Section_Footer };
    Section  currentSection{Section_None};

    // Active drag target (node being dragged, 0 = none)
    Id       draggingNodeId{0};
    ImVec2   dragStartNodePos{};
    ImVec2   dragStartMouseCanvas{};

    // -----------------------------
    // Connection creation state machine (M2)
    // -----------------------------
    Id          draggingFromSlot{0};          // Non-zero → user is dragging a link from this slot
    Id          currentHoveredSlot{0};        // Slot the mouse is currently on this frame
    Id          currentHoveredNode{0};        // Node the mouse is currently on this frame
    Id          currentHoveredLink{0};        // Link the mouse is currently on this frame
    bool        connAcceptedThisFrame{false}; // AcceptNewLink called this frame
    bool        connNewNodeAcceptedThisFrame{false}; // AcceptNewNodeFromSlot called this frame
    ImU32       connAcceptColor{0};           // Color user passed to AcceptNewLink/NewNode
    const char* connRejectReason{nullptr};    // RejectNewLink reason (pointer assumed stable for the frame)
    bool        connCommitThisFrame{false};   // Set when mouse released AND accepted this frame
    bool        connNewNodeCommitThisFrame{false}; // Set when drop-on-empty committed this frame

    // Global "selected link" for standalone (outside-of-graph) link queries.
    Id          standaloneSelectedLink{0};

    // Context-menu requests (right-click on a specific item this frame).
    // Reset every NewFrame; populated by EndNode / EndSlot / Link.
    Id          ctxMenuNodeId{0};
    Id          ctxMenuSlotId{0};
    Id          ctxMenuLinkId{0};

    // -----------------------------
    // Delete state machine
    // -----------------------------
    bool        deleteScopeOpen{false};
    std::deque<Id> pendingDeleteLinks;
    std::deque<Id> pendingDeleteNodes;
    Id          currentDeleteCandidate{0};      // last id returned by QueryDeletedLink/Node
    int         currentDeleteKind{0};           // 0 = none, 1 = link, 2 = node
    // Snapshot of accepted ids in this BeginDelete/EndDelete scope — used to
    // remove them from the selection at EndDelete (so the host doesn't have to).
    std::vector<Id> acceptedDeleteLinks;
    std::vector<Id> acceptedDeleteNodes;

    // -----------------------------
    // Shortcut state machine (Ctrl+C/V/X/D/A)
    // -----------------------------
    bool        shortcutScopeOpen{false};
    bool        shortcutCopyFired{false};
    bool        shortcutPasteFired{false};
    bool        shortcutCutFired{false};
    bool        shortcutDuplicateFired{false};
    bool        shortcutSelectAllFired{false};
    // Snapshot taken when a shortcut fires — host can read it via
    // GetActionContextNodes/Links the same frame.
    std::vector<Id> actionContextNodes;
    std::vector<Id> actionContextLinks;

    // -----------------------------
    // Slot dot pivot override (current slot only)
    // -----------------------------
    ImVec2      slotPivotAlignment{-1.0f, -1.0f};  // <0 = use role default
    ImVec2      slotPivotOffset{0.0f, 0.0f};

    // -----------------------------
    // BeginAlign / EndAlign layout container
    // -----------------------------
    // Persistent: keyed by ImGui scope ID, remembers last frame's group width
    // so this frame can compute the right indent.
    std::unordered_map<ImGuiID, AlignSlot> alignSlots;
    // Stack of currently-open BeginAlign IDs (to support nesting/sequential).
    std::vector<ImGuiID> alignStack;
    // Per-frame counter for auto-generating unique IDs across multiple
    // BeginAlign calls in the same ImGui scope.
    int         alignCallCounter{0};

    // -----------------------------
    // Box-select state (started by left-drag from empty canvas)
    // -----------------------------
    // Two phases: pendingBgClick after mouse-down on empty canvas, then
    // boxSelectActive once the drag distance crosses the threshold. On
    // release, either commit the box (if active) or treat it as a plain
    // bg-click (if not). Coordinates are LOCAL-SPACE (i.e. canvas-space
    // when inside BeginCanvas) — same space used by node lastNodeRect and
    // link cached endpoints.
    bool        pendingBgClick{false};
    ImVec2      pendingBgClickPos{};               // local-space (au moment du clic)
    ImVec2      pendingBgClickPosScreen{};         // screen-space (pour le seuil de drag)
    bool        boxSelectActive{false};
    Id          boxSelectGraphId{0};
    ImVec2      boxSelectStart{};                  // local-space

    // Set by ImNodal::NewFrame() to ImGui::GetFrameCount() so repeated calls
    // within the same frame are idempotent.
    int         lastFrameReset{-1};
};

namespace {

static Context* g_currentCtx{nullptr};

// Access the current context with an assert. Every public Begin/End/query
// routes through this so "no context" fails loud instead of corrupting state.
inline Context& s_getCtx() {
    IM_ASSERT(g_currentCtx != nullptr && "ImNodal: no current context. Call CreateContext() + SetCurrentContext() first.");
    return *g_currentCtx;
}

// Reset all per-frame state. Invoked by the public ImNodal::NewFrame().
// Idempotent within a frame: the lastFrameReset guard means extra calls in the
// same frame are no-ops.
static void s_doNewFrame(Context& arCtx) {
    const int frame = ImGui::GetFrameCount();
    if (arCtx.lastFrameReset == frame) return;
    arCtx.lastFrameReset = frame;

    arCtx.currentHoveredSlot         = 0;
    arCtx.currentHoveredNode         = 0;
    arCtx.currentHoveredLink         = 0;
    arCtx.connAcceptedThisFrame      = false;
    arCtx.connNewNodeAcceptedThisFrame = false;
    arCtx.connAcceptColor            = 0;
    arCtx.connRejectReason           = nullptr;
    arCtx.connCommitThisFrame        = false;
    arCtx.connNewNodeCommitThisFrame = false;

    arCtx.ctxMenuNodeId = 0;
    arCtx.ctxMenuSlotId = 0;
    arCtx.ctxMenuLinkId = 0;

    arCtx.shortcutCopyFired      = false;
    arCtx.shortcutPasteFired     = false;
    arCtx.shortcutCutFired       = false;
    arCtx.shortcutDuplicateFired = false;
    arCtx.shortcutSelectAllFired = false;

    arCtx.slotPivotAlignment = ImVec2(-1.0f, -1.0f);
    arCtx.slotPivotOffset    = ImVec2(0.0f, 0.0f);

    arCtx.alignCallCounter = 0;

    for (auto& kv : arCtx.slots) {
        kv.second.connected = false;
        kv.second.ctxMenuRequested = false;
    }
    for (auto& kv : arCtx.links) {
        kv.second.hovered = false;
        kv.second.clicked = false;
        kv.second.doubleClicked = false;
        kv.second.ctxMenuRequested = false;
        kv.second.bezierCached = false;
    }
    for (auto& kv : arCtx.nodes) {
        kv.second.ctxMenuRequested = false;
    }
    for (auto& kv : arCtx.graphs) {
        kv.second.selectionChangedThisFrame = false;
    }

    // Safety: if the mouse has been up for strictly MORE than one frame and the
    // drag state is still set, the user forgot a matching EndConnectionCreate
    // (or their BeginConnectionCreate was skipped for scope reasons). Clear it.
    // We must NOT clear on the release frame itself: EndConnectionCreate needs
    // IsMouseReleased==true this frame to run its commit logic.
    const bool mouseUp       = !ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool onReleaseFrame = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (arCtx.draggingFromSlot != 0 && mouseUp && !onReleaseFrame) {
        arCtx.draggingFromSlot = 0;
    }
}

// True if the current drag (if any) originated from a slot whose scope
// matches the currently open scope — graph id when inside BeginGraph, or 0
// for standalone slots. Used to gate connection-creation queries/commits so
// a drag started in one scope doesn't fire handlers in another.
static bool s_isDragInCurrentScope(const Context& arCtx) {
    if (arCtx.draggingFromSlot == 0) return false;
    auto it = arCtx.slots.find(arCtx.draggingFromSlot);
    if (it == arCtx.slots.end()) return false;
    const Id dragScope    = it->second.graphId;
    const Id currentScope = arCtx.graphActive ? arCtx.currentGraphId : (Id)0;
    return dragScope == currentScope;
}

inline ImVec2 s_selectPositive(const ImVec2& arA, const ImVec2& arB) {
    return ImVec2(arA.x > 0.0f ? arA.x : arB.x, arA.y > 0.0f ? arA.y : arB.y);
}

// ---------------------------------------------------------------------
// Coordinate helpers
// ---------------------------------------------------------------------
inline ImVec2 s_canvasToScreen(const Context& arCtx, const ImVec2& aP) {
    return aP * arCtx.scale + arCtx.viewTransformPos;
}
inline ImVec2 s_screenToCanvas(const Context& arCtx, const ImVec2& aP) {
    return (aP - arCtx.viewTransformPos) * arCtx.invScale;
}

inline ImRect s_calcViewRect(const Context& arCtx) {
    ImRect r;
    r.Min = ImVec2(-arCtx.origin.x, -arCtx.origin.y) * arCtx.invScale;
    r.Max = (arCtx.widgetSize - arCtx.origin) * arCtx.invScale;
    return r;
}

inline void s_updateViewTransformPos(Context& arCtx) {
    arCtx.viewTransformPos = arCtx.origin + arCtx.widgetPos;
}

// ---------------------------------------------------------------------
// Local-space enter/leave — the core of the zoom trick.
// While in local space, ImGui draws in canvas coordinates (unscaled).
// At leave, we bake vertices back into screen space applying scale.
// ---------------------------------------------------------------------

static void s_saveInputState(Context& arCtx) {
    const auto& rIo = ImGui::GetIO();
    arCtx.mousePosBackup     = rIo.MousePos;
    arCtx.mousePosPrevBackup = rIo.MousePosPrev;
    for (int i = 0; i < IM_ARRAYSIZE(arCtx.mouseClickedPosBackup); ++i) {
        arCtx.mouseClickedPosBackup[i] = rIo.MouseClickedPos[i];
    }
}

static void s_restoreInputState(Context& arCtx) {
    auto& rIo = ImGui::GetIO();
    rIo.MousePos     = arCtx.mousePosBackup;
    rIo.MousePosPrev = arCtx.mousePosPrevBackup;
    for (int i = 0; i < IM_ARRAYSIZE(arCtx.mouseClickedPosBackup); ++i) {
        rIo.MouseClickedPos[i] = arCtx.mouseClickedPosBackup[i];
    }
}

static void s_saveViewportState(Context& arCtx) {
    auto* const pWindow   = ImGui::GetCurrentWindow();
    auto* const pViewport = ImGui::GetWindowViewport();
    arCtx.windowPosBackup        = pWindow->Pos;
    arCtx.viewportPosBackup      = pViewport->Pos;
    arCtx.viewportSizeBackup     = pViewport->Size;
    arCtx.viewportWorkPosBackup  = pViewport->WorkPos;
    arCtx.viewportWorkSizeBackup = pViewport->WorkSize;
}

static void s_restoreViewportState(Context& arCtx) {
    auto* const pWindow   = ImGui::GetCurrentWindow();
    auto* const pViewport = ImGui::GetWindowViewport();
    pWindow->Pos        = arCtx.windowPosBackup;
    pViewport->Pos      = arCtx.viewportPosBackup;
    pViewport->Size     = arCtx.viewportSizeBackup;
    pViewport->WorkPos  = arCtx.viewportWorkPosBackup;
    pViewport->WorkSize = arCtx.viewportWorkSizeBackup;
}

static void s_enterLocalSpace(Context& arCtx) {
    // Resolve clipped clip rect BEFORE opening a new command.
    ImGui::PushClipRect(arCtx.widgetPos, arCtx.widgetPos + arCtx.widgetSize, true);
    auto clippedClipRect = arCtx.drawList->_ClipRectStack.back();
    ImGui::PopClipRect();

    arCtx.cmdBufferSize  = ImMax(arCtx.drawList->CmdBuffer.Size - 1, 0);
    arCtx.startVertexIdx = arCtx.drawList->_VtxCurrentIdx + arCtx.drawList->_CmdHeader.VtxOffset;

    auto* const pWindow   = ImGui::GetCurrentWindow();
    auto* const pViewport = ImGui::GetWindowViewport();
    pWindow->Pos = ImVec2(0.0f, 0.0f);

    // Transform viewport to local space
    ImVec2 vpMin = arCtx.viewportPosBackup;
    ImVec2 vpMax = arCtx.viewportPosBackup + arCtx.viewportSizeBackup;
    vpMin = (vpMin - arCtx.viewTransformPos) * arCtx.invScale;
    vpMax = (vpMax - arCtx.viewTransformPos) * arCtx.invScale;
    pViewport->Pos      = vpMin;
    pViewport->Size     = vpMax - vpMin;
    pViewport->WorkPos  = arCtx.viewportWorkPosBackup  * arCtx.invScale;
    pViewport->WorkSize = arCtx.viewportWorkSizeBackup * arCtx.invScale;

    // Clip rect in local space
    clippedClipRect.x = (clippedClipRect.x - arCtx.viewTransformPos.x) * arCtx.invScale;
    clippedClipRect.y = (clippedClipRect.y - arCtx.viewTransformPos.y) * arCtx.invScale;
    clippedClipRect.z = (clippedClipRect.z - arCtx.viewTransformPos.x) * arCtx.invScale;
    clippedClipRect.w = (clippedClipRect.w - arCtx.viewTransformPos.y) * arCtx.invScale;
    ImGui::PushClipRect(ImVec2(clippedClipRect.x, clippedClipRect.y),
                        ImVec2(clippedClipRect.z, clippedClipRect.w),
                        false);

    // Mouse in local space
    auto& rIo = ImGui::GetIO();
    rIo.MousePos     = (arCtx.mousePosBackup     - arCtx.viewTransformPos) * arCtx.invScale;
    rIo.MousePosPrev = (arCtx.mousePosPrevBackup - arCtx.viewTransformPos) * arCtx.invScale;
    for (int i = 0; i < IM_ARRAYSIZE(arCtx.mouseClickedPosBackup); ++i) {
        rIo.MouseClickedPos[i] = (arCtx.mouseClickedPosBackup[i] - arCtx.viewTransformPos) * arCtx.invScale;
    }

    arCtx.viewRect = s_calcViewRect(arCtx);

    // Scale AA thickness so strokes render crisp at any zoom
    arCtx.lastFringeScale = arCtx.drawList->_FringeScale;
    arCtx.drawList->_FringeScale *= arCtx.invScale;
}

static void s_leaveLocalSpace(Context& arCtx) {
    IM_ASSERT(arCtx.drawList->_Splitter._Current == arCtx.expectedChannel && "Unbalanced channel splitter on leave local space");

    // Bake new vertices: canvas-space -> screen-space
    auto* pVtx    = arCtx.drawList->VtxBuffer.Data + arCtx.startVertexIdx;
    auto* pVtxEnd = arCtx.drawList->VtxBuffer.Data + arCtx.drawList->_VtxCurrentIdx + arCtx.drawList->_CmdHeader.VtxOffset;

    if (arCtx.scale != 1.0f) {
        while (pVtx < pVtxEnd) {
            pVtx->pos.x = pVtx->pos.x * arCtx.scale + arCtx.viewTransformPos.x;
            pVtx->pos.y = pVtx->pos.y * arCtx.scale + arCtx.viewTransformPos.y;
            ++pVtx;
        }
        for (int i = arCtx.cmdBufferSize; i < arCtx.drawList->CmdBuffer.size(); ++i) {
            auto& rCmd = arCtx.drawList->CmdBuffer[i];
            rCmd.ClipRect.x = rCmd.ClipRect.x * arCtx.scale + arCtx.viewTransformPos.x;
            rCmd.ClipRect.y = rCmd.ClipRect.y * arCtx.scale + arCtx.viewTransformPos.y;
            rCmd.ClipRect.z = rCmd.ClipRect.z * arCtx.scale + arCtx.viewTransformPos.x;
            rCmd.ClipRect.w = rCmd.ClipRect.w * arCtx.scale + arCtx.viewTransformPos.y;
        }
    } else {
        while (pVtx < pVtxEnd) {
            pVtx->pos.x += arCtx.viewTransformPos.x;
            pVtx->pos.y += arCtx.viewTransformPos.y;
            ++pVtx;
        }
        for (int i = arCtx.cmdBufferSize; i < arCtx.drawList->CmdBuffer.size(); ++i) {
            auto& rCmd = arCtx.drawList->CmdBuffer[i];
            rCmd.ClipRect.x += arCtx.viewTransformPos.x;
            rCmd.ClipRect.y += arCtx.viewTransformPos.y;
            rCmd.ClipRect.z += arCtx.viewTransformPos.x;
            rCmd.ClipRect.w += arCtx.viewTransformPos.y;
        }
    }

    arCtx.drawList->_FringeScale = arCtx.lastFringeScale;

    ImGui::PopClipRect();
    s_restoreInputState(arCtx);
    s_restoreViewportState(arCtx);
}

// ---------------------------------------------------------------------
// Interaction: pan + zoom. Called from BeginCanvas while in local space.
// ---------------------------------------------------------------------

static void s_managePan(Context& arCtx) {
    const auto btn = arCtx.settings.panButton;

    if ((arCtx.isPanning || ImGui::IsWindowHovered()) && ImGui::IsMouseDragging(btn, 0.0f)) {
        if (!arCtx.isPanning) {
            arCtx.isPanning = true;
            arCtx.panStartOrigin = arCtx.origin;
        }
        const ImVec2 delta = ImGui::GetMouseDragDelta(btn, 0.0f) * arCtx.scale;
        SetCanvasView(arCtx.panStartOrigin + delta, arCtx.scale);
    } else if (arCtx.isPanning && !ImGui::IsMouseDown(btn)) {
        arCtx.isPanning = false;
    }
}

static void s_manageZoom(Context& arCtx) {
    const auto& rIo = ImGui::GetIO();
    const float wheel = rIo.MouseWheel;
    const bool resetKey = (arCtx.settings.resetZoomKey != ImGuiKey_None) && ImGui::IsKeyPressed(arCtx.settings.resetZoomKey);

    if (wheel == 0.0f && !resetKey) {
        return;
    }

    if (resetKey) {
        SetCanvasView(arCtx.widgetSize * 0.5f, 1.0f);
        return;
    }

    const float newScale = ImClamp(
        arCtx.scale + wheel * arCtx.settings.zoomStep,
        arCtx.settings.zoomMin,
        arCtx.settings.zoomMax);
    if (newScale == arCtx.scale) {
        return;
    }

    // Zoom anchored on cursor: same canvas point stays under mouse before/after.
    // mousePosBackup is cursor in screen space (saved before entering local space).
    const ImVec2 mouseCanvas = (arCtx.mousePosBackup - arCtx.viewTransformPos) * arCtx.invScale;
    const ImVec2 newOrigin   = arCtx.mousePosBackup - arCtx.widgetPos - mouseCanvas * newScale;
    SetCanvasView(newOrigin, newScale);
}

static void s_manageInteractions(Context& arCtx) {
    // Background interaction flags (bgClicked / bgDoubleClicked / bgCtxMenuRequested)
    // are NOT reset here. They are owned entirely by EndCanvas: computed there
    // (set to true or to false depending on "on empty" detection), and remain
    // stable across the whole next frame so user code can read them INSIDE
    // their Begin/End scope. Resetting them at Begin would create a window
    // where End sets the flag and the next Begin immediately clears it before
    // user code gets a chance to read it.
    if (!ImGui::IsWindowHovered()) {
        return;
    }
    s_manageZoom(arCtx);
    s_managePan(arCtx);
}

}  // namespace

// =====================================================================
// Public API
// =====================================================================

// -----------------------------
// Context lifecycle
// -----------------------------
IMNODAL_API Context* CreateContext() {
    auto* const pCtx = IM_NEW(Context)();
    if (g_currentCtx == nullptr) {
        g_currentCtx = pCtx;  // Convenience: the first created context becomes current
    }
    return pCtx;
}

IMNODAL_API void DestroyContext(Context* apCtx) {
    Context* const pTarget = (apCtx != nullptr) ? apCtx : g_currentCtx;
    if (pTarget == nullptr) {
        return;
    }
    IM_ASSERT(pTarget->active == false && "DestroyContext called while a Begin/End scope is still open");
    if (g_currentCtx == pTarget) {
        g_currentCtx = nullptr;
    }
    IM_DELETE(pTarget);
}

IMNODAL_API Context* GetCurrentContext() {
    return g_currentCtx;
}

IMNODAL_API void SetCurrentContext(Context* apCtx) {
    g_currentCtx = apCtx;
}

IMNODAL_API void NewFrame() {
    s_doNewFrame(s_getCtx());
}

// -----------------------------
// Debug
// -----------------------------
IMNODAL_API bool DebugCheckVersion(const char* aVersion, size_t aSettingsSize) {
    const bool vers = (aVersion != nullptr) && (std::strcmp(aVersion, IMNODAL_VERSION) == 0);
    const bool sz   = (aSettingsSize == sizeof(CanvasSettings));
    IM_ASSERT(vers && "ImNodal version mismatch");
    IM_ASSERT(sz   && "ImNodal CanvasSettings size mismatch");
    return vers && sz;
}

IMNODAL_API bool BeginCanvas(const char* aId, const ImVec2& aSize, const CanvasSettings& arSettings) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.active == false && "BeginCanvas called twice without EndCanvas");
    IM_ASSERT(aId != nullptr && "BeginCanvas: id must be non-null");

    rCtx.settings = arSettings;

    rCtx.widgetPos  = ImGui::GetCursorScreenPos();
    rCtx.widgetSize = s_selectPositive(aSize, ImGui::GetContentRegionAvail());
    rCtx.widgetRect = ImRect(rCtx.widgetPos, rCtx.widgetPos + rCtx.widgetSize);
    rCtx.drawList   = ImGui::GetWindowDrawList();

    // Auto-center the view on the first successful Begin
    if (!rCtx.viewInitialized) {
        rCtx.origin   = rCtx.widgetSize * 0.5f;
        rCtx.scale    = 1.0f;
        rCtx.invScale = 1.0f;
        rCtx.viewInitialized = true;
    }
    s_updateViewTransformPos(rCtx);

    const ImGuiID id = ImGui::GetID(aId);
    if (ImGui::IsClippedEx(rCtx.widgetRect, id)) {
        return false;
    }

    rCtx.expectedChannel       = rCtx.drawList->_Splitter._Current;
    rCtx.windowCursorMaxBackup = ImGui::GetCurrentWindow()->DC.CursorMaxPos;

    // Block window-move ONLY when the mouse is inside the canvas area. This
    // preserves titlebar drag (mouse on titlebar → no NoMove → move works),
    // while clicks/drags inside the canvas never start a window move.
    // ImGui's window-move decision runs at EndFrame and reads the current
    // flags, so setting it here takes effect on this frame's mouse-press.
    if (ImGui::IsMouseHoveringRect(rCtx.widgetPos, rCtx.widgetPos + rCtx.widgetSize)) {
        ImGui::GetCurrentWindow()->Flags |= ImGuiWindowFlags_NoMove;
    }

    s_saveInputState(rCtx);
    s_saveViewportState(rCtx);

    s_enterLocalSpace(rCtx);
    rCtx.active = true;

    // Push a canvas-scoped ID so any user widget emitted between BeginCanvas
    // and EndCanvas lives in its own ID namespace (avoids clashes with widgets
    // outside the canvas, and with widgets in another canvas in the same frame).
    ImGui::PushID(aId);

    // Mouse is now in local space. Interactions run here.
    s_manageInteractions(rCtx);

    if (rCtx.settings.drawGrid) {
        DrawCanvasGrid();
    }

    // Reset ImGui cursor so user drawing starts at canvas origin (0,0).
    ImGui::SetCursorScreenPos(ImVec2(0.0f, 0.0f));

    return true;
}

IMNODAL_API void EndCanvas() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.active == true && "EndCanvas without matching BeginCanvas");
    IM_ASSERT(rCtx.drawList->_Splitter._Current == rCtx.expectedChannel && "Unbalanced channel splitter inside canvas scope");
    IM_ASSERT(rCtx.suspendCounter == 0 && "Unmatched SuspendCanvas/ResumeCanvas");

    // Match the PushID(aId) at BeginCanvas.
    ImGui::PopID();

    s_leaveLocalSpace(rCtx);
    ImGui::GetCurrentWindow()->DC.CursorMaxPos = rCtx.windowCursorMaxBackup;

    // Background interaction flags: computed now that mouse is back in screen
    // space, all user items are emitted, and our own layout Dummy is not yet.
    rCtx.hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(rCtx.widgetRect.Min, rCtx.widgetRect.Max);
    // "onEmpty" = vraiment sur du fond : pas d'item ImGui hovered (couvre
    // nodes/slots qui ont des ItemAdd) ET pas de LIEN hovered (les liens
    // sont dessines via DrawList sans ItemAdd, donc IsAnyItemHovered les
    // ignore — sans ce check, double-cliquer sur un lien ferait aussi fire
    // bgDoubleClicked et declencherait l'action "fit-to-view" du host).
    const bool onEmpty = rCtx.hovered &&
                         !ImGui::IsAnyItemHovered() &&
                         rCtx.currentHoveredLink == 0 &&
                         !rCtx.isPanning;
    if (onEmpty) {
        rCtx.bgClicked          = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        rCtx.bgDoubleClicked    = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        rCtx.bgCtxMenuRequested = ImGui::IsMouseClicked(rCtx.settings.contextMenuButton);
    } else {
        rCtx.bgClicked = false;
        rCtx.bgDoubleClicked = false;
        rCtx.bgCtxMenuRequested = false;
    }

    // Advance ImGui layout cursor.
    ImGui::SetCursorScreenPos(rCtx.widgetPos);
    ImGui::Dummy(rCtx.widgetSize);

    rCtx.active = false;
}

// -----------------------------
// Queries
// -----------------------------
IMNODAL_API bool IsCanvasHovered()                 { return s_getCtx().hovered; }
IMNODAL_API bool IsCanvasBackgroundClicked()       { return s_getCtx().bgClicked; }
IMNODAL_API bool IsCanvasBackgroundDoubleClicked() { return s_getCtx().bgDoubleClicked; }
IMNODAL_API bool IsCanvasContextMenuRequested()    { return s_getCtx().bgCtxMenuRequested; }
IMNODAL_API bool IsCanvasPanning()                 { return s_getCtx().isPanning; }

// -----------------------------
// View
// -----------------------------
IMNODAL_API ImVec2 GetCanvasOrigin() { return s_getCtx().origin; }
IMNODAL_API float  GetCanvasScale()  { return s_getCtx().scale; }

IMNODAL_API void SetCanvasView(const ImVec2& aOrigin, float aScale) {
    Context& rCtx = s_getCtx();
    const bool reenter = rCtx.active && rCtx.suspendCounter == 0;
    if (reenter) {
        s_leaveLocalSpace(rCtx);
    }
    const bool originChanged = (rCtx.origin.x != aOrigin.x) || (rCtx.origin.y != aOrigin.y);
    if (originChanged) {
        rCtx.origin = aOrigin;
        s_updateViewTransformPos(rCtx);
    }
    if (rCtx.scale != aScale) {
        rCtx.scale    = aScale;
        rCtx.invScale = (aScale != 0.0f) ? 1.0f / aScale : 0.0f;
    }
    if (reenter) {
        s_enterLocalSpace(rCtx);
    }
}

IMNODAL_API void ResetCanvasView() {
    Context& rCtx = s_getCtx();
    SetCanvasView(rCtx.widgetSize * 0.5f, 1.0f);
}

IMNODAL_API void CenterCanvasOn(const ImVec2& aCanvasPos) {
    Context& rCtx = s_getCtx();
    const ImVec2 localCenter  = s_screenToCanvas(rCtx, rCtx.widgetPos + rCtx.widgetSize * 0.5f);
    const ImVec2 localOffset  = aCanvasPos - localCenter;
    const ImVec2 screenOffset = localOffset * rCtx.scale;
    SetCanvasView(rCtx.origin - screenOffset, rCtx.scale);
}

IMNODAL_API void ZoomCanvasToRect(const ImVec2& aMin, const ImVec2& aMax, float aMarginRatio) {
    Context& rCtx = s_getCtx();
    ImVec2 size = aMax - aMin;
    if (size.x <= 0.0f || size.y <= 0.0f || rCtx.widgetSize.x <= 0.0f || rCtx.widgetSize.y <= 0.0f) {
        return;
    }
    // Expand the target rect by margin on every side.
    const float extend = ImMax(size.x, size.y) * aMarginRatio * 0.5f;
    const ImVec2 growMin = aMin - ImVec2(extend, extend);
    const ImVec2 growMax = aMax + ImVec2(extend, extend);
    size = growMax - growMin;

    const float widgetAR = rCtx.widgetSize.x / rCtx.widgetSize.y;
    const float rectAR   = size.x / size.y;

    float newScale;
    ImVec2 newOrigin;
    if (rectAR > widgetAR) {
        newScale  = rCtx.widgetSize.x / size.x;
        newOrigin = growMin * -newScale;
        newOrigin.y += (rCtx.widgetSize.y - size.y * newScale) * 0.5f;
    } else {
        newScale  = rCtx.widgetSize.y / size.y;
        newOrigin = growMin * -newScale;
        newOrigin.x += (rCtx.widgetSize.x - size.x * newScale) * 0.5f;
    }
    newScale = ImClamp(newScale, rCtx.settings.zoomMin, rCtx.settings.zoomMax);
    SetCanvasView(newOrigin, newScale);
}

// -----------------------------
// Coords
// -----------------------------
IMNODAL_API ImVec2 CanvasToScreen(const ImVec2& aP)  { return s_canvasToScreen(s_getCtx(), aP); }
IMNODAL_API ImVec2 ScreenToCanvas(const ImVec2& aP)  { return s_screenToCanvas(s_getCtx(), aP); }
IMNODAL_API ImVec2 CanvasToScreenV(const ImVec2& aV) { return aV * s_getCtx().scale; }
IMNODAL_API ImVec2 ScreenToCanvasV(const ImVec2& aV) { return aV * s_getCtx().invScale; }

// -----------------------------
// Rects
// -----------------------------
IMNODAL_API ImRect GetCanvasRect()     { return s_getCtx().widgetRect; }
IMNODAL_API ImRect GetCanvasViewRect() { return s_getCtx().viewRect; }

// -----------------------------
// Suspend / Resume
// -----------------------------
IMNODAL_API void SuspendCanvas() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.active && "SuspendCanvas outside of Begin/EndCanvas");
    IM_ASSERT(rCtx.drawList->_Splitter._Current == rCtx.expectedChannel && "SuspendCanvas: unbalanced channel splitter");
    if (rCtx.suspendCounter == 0) {
        s_leaveLocalSpace(rCtx);
    }
    ++rCtx.suspendCounter;
}

IMNODAL_API void ResumeCanvas() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.active && "ResumeCanvas outside of Begin/EndCanvas");
    IM_ASSERT(rCtx.suspendCounter > 0 && "Unmatched ResumeCanvas");
    if (--rCtx.suspendCounter == 0) {
        s_enterLocalSpace(rCtx);
    }
}

IMNODAL_API bool IsCanvasSuspended() {
    return s_getCtx().suspendCounter > 0;
}

// -----------------------------
// Grid
// -----------------------------
IMNODAL_API void DrawCanvasGrid() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.active && "DrawCanvasGrid outside of Begin/EndCanvas");

    const auto& rCfg     = rCtx.settings;
    auto* const pDrawList= rCtx.drawList;
    const ImVec2 offset  = rCtx.origin * rCtx.invScale;
    const ImVec2 winPos  = rCtx.viewRect.Min;
    const ImVec2 size    = rCtx.viewRect.GetSize();

    // Major lines
    for (float x = std::fmod(offset.x, rCfg.gridSize.x); x < size.x; x += rCfg.gridSize.x) {
        pDrawList->AddLine(ImVec2(x, 0.0f) + winPos, ImVec2(x, size.y) + winPos, rCfg.gridColor);
    }
    for (float y = std::fmod(offset.y, rCfg.gridSize.y); y < size.y; y += rCfg.gridSize.y) {
        pDrawList->AddLine(ImVec2(0.0f, y) + winPos, ImVec2(size.x, y) + winPos, rCfg.gridColor);
    }

    // Minor subdivisions
    if (rCfg.gridSubdivs.x != 0.0f && rCfg.gridSubdivs.y != 0.0f) {
        const ImVec2 sub = rCfg.gridSize / rCfg.gridSubdivs;
        for (float x = std::fmod(offset.x, sub.x); x < size.x; x += sub.x) {
            pDrawList->AddLine(ImVec2(x, 0.0f) + winPos, ImVec2(x, size.y) + winPos, rCfg.subGridColor);
        }
        for (float y = std::fmod(offset.y, sub.y); y < size.y; y += sub.y) {
            pDrawList->AddLine(ImVec2(0.0f, y) + winPos, ImVec2(size.x, y) + winPos, rCfg.subGridColor);
        }
    }
}

// =====================================================================
// Graph layer — implementation (M1)
// =====================================================================

namespace {

// Draw list channel layout (per active BeginGraph).
// Channels are merged in index order, so lower index = rendered first =
// at the bottom of the z-stack. Links sit BELOW node bodies so a wire
// entering a node disappears under it (only visible between nodes).
//   UserBg : where GetNodeBackgroundDrawList() drops user shapes — above
//            the node body but below the user widgets.
//   UserFg : where GetNodeForegroundDrawList() drops user shapes — above
//            the user widgets but below the drag-preview overlay.
enum GraphChannel {
    GC_Links      = 0,  // links drawn under everything else
    GC_Background = 1,  // node bg + border + slot dots
    GC_UserBg     = 2,  // user-drawn overlays under content
    GC_Content    = 3,  // user widgets inside nodes
    GC_UserFg     = 4,  // user-drawn overlays above content
    GC_Overlay    = 5,  // box-select, drag-preview link (topmost)
    GC_Count      = 6,
};

// Hash Id(u64) -> ImGuiID(u32). ImGui uses u32 internally; we need to push
// scoped IDs from our u64s safely.
static ImGuiID s_imguiId(Id aId) {
    return ImHashData(&aId, sizeof(aId), 0);
}

// Bring the draw list to a given graph channel (asserts graph is active).
static void s_setChannel(Context& arCtx, int aChannel) {
    IM_ASSERT(arCtx.graphActive && "s_setChannel requires an active graph (BeginGraph scope)");
    arCtx.drawList->ChannelsSetCurrent(aChannel);
}

// -----------------------------
// Selection helpers
// -----------------------------
// Single-select: clears all selection, then adds aId.
// Toggle:        flips aId's membership; other items keep their state.
// Both update lastSelectedNode/Link so the legacy single-id getters return a
// stable "most recently touched" id, and set selectionChangedThisFrame when
// the set actually changed.
static void s_selectNode(GraphState& arGraph, Id aId, bool aToggle) {
    if (aToggle) {
        if (arGraph.selectedNodes.erase(aId) != 0) {
            if (arGraph.lastSelectedNode == aId) {
                arGraph.lastSelectedNode = arGraph.selectedNodes.empty() ? 0 : *arGraph.selectedNodes.begin();
            }
        } else {
            arGraph.selectedNodes.insert(aId);
            arGraph.lastSelectedNode = aId;
        }
        arGraph.selectionChangedThisFrame = true;
    } else {
        const bool wasOnly  = arGraph.selectedNodes.size() == 1 && *arGraph.selectedNodes.begin() == aId;
        const bool noLinks  = arGraph.selectedLinks.empty();
        if (!wasOnly || !noLinks) {
            arGraph.selectedNodes.clear();
            arGraph.selectedLinks.clear();
            arGraph.selectedNodes.insert(aId);
            arGraph.selectionChangedThisFrame = true;
        }
        arGraph.lastSelectedNode = aId;
        arGraph.lastSelectedLink = 0;
    }
}

static void s_selectLink(GraphState& arGraph, Id aId, bool aToggle) {
    if (aToggle) {
        if (arGraph.selectedLinks.erase(aId) != 0) {
            if (arGraph.lastSelectedLink == aId) {
                arGraph.lastSelectedLink = arGraph.selectedLinks.empty() ? 0 : *arGraph.selectedLinks.begin();
            }
        } else {
            arGraph.selectedLinks.insert(aId);
            arGraph.lastSelectedLink = aId;
        }
        arGraph.selectionChangedThisFrame = true;
    } else {
        const bool wasOnly  = arGraph.selectedLinks.size() == 1 && *arGraph.selectedLinks.begin() == aId;
        const bool noNodes  = arGraph.selectedNodes.empty();
        if (!wasOnly || !noNodes) {
            arGraph.selectedNodes.clear();
            arGraph.selectedLinks.clear();
            arGraph.selectedLinks.insert(aId);
            arGraph.selectionChangedThisFrame = true;
        }
        arGraph.lastSelectedNode = 0;
        arGraph.lastSelectedLink = aId;
    }
}

static void s_clearSelection(GraphState& arGraph) {
    if (!arGraph.selectedNodes.empty() || !arGraph.selectedLinks.empty()) {
        arGraph.selectedNodes.clear();
        arGraph.selectedLinks.clear();
        arGraph.selectionChangedThisFrame = true;
    }
    arGraph.lastSelectedNode = 0;
    arGraph.lastSelectedLink = 0;
}

// True if the multi-select modifier (default: Shift) is currently held.
static bool s_multiSelectHeld(const GraphState& arGraph) {
    return arGraph.settings.allowMultiSelect && ImGui::IsKeyDown(arGraph.settings.multiSelectKey);
}

// -----------------------------
// Box-select hit helpers (intersection-based)
// -----------------------------

// Slab method : true if the line segment (aA, aB) intersects the AABB
// (aMin, aMax). Returns true if either endpoint is inside, OR if the segment
// crosses the box. Used by the box-select to test bezier samples chord by
// chord — catches cases where the curve dips into the box between samples.
static bool s_segmentIntersectsRect(const ImVec2& aA, const ImVec2& aB,
                                    const ImVec2& aMin, const ImVec2& aMax) {
    const auto inside = [&](const ImVec2& p) {
        return p.x >= aMin.x && p.x <= aMax.x && p.y >= aMin.y && p.y <= aMax.y;
    };
    if (inside(aA) || inside(aB)) return true;
    const ImVec2 d(aB.x - aA.x, aB.y - aA.y);
    float tMin = 0.0f, tMax = 1.0f;
    auto clip = [&](float dirP, float minP, float maxP, float startP) {
        if (std::fabs(dirP) < 1e-6f) return !(startP < minP || startP > maxP);
        float t1 = (minP - startP) / dirP;
        float t2 = (maxP - startP) / dirP;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
        if (t1 > tMin) tMin = t1;
        if (t2 < tMax) tMax = t2;
        return tMin <= tMax;
    };
    if (!clip(d.x, aMin.x, aMax.x, aA.x)) return false;
    if (!clip(d.y, aMin.y, aMax.y, aA.y)) return false;
    return tMin <= 1.0f && tMax >= 0.0f;
}

// Sample the cubic bezier into N chords and return true as soon as one
// chord intersects the AABB. Catches all curve-vs-box overlaps including
// when the curve only clips a corner of the box.
static bool s_bezierIntersectsRect(const ImVec2& aP0, const ImVec2& aP1,
                                   const ImVec2& aP2, const ImVec2& aP3,
                                   const ImVec2& aMin, const ImVec2& aMax) {
    constexpr int kSamples = 32;
    ImVec2 prev = aP0;
    for (int i = 1; i <= kSamples; ++i) {
        const float t = (float)i / (float)kSamples;
        const float u = 1.0f - t;
        const ImVec2 pt =
            aP0 * (u * u * u) +
            aP1 * (3.0f * u * u * t) +
            aP2 * (3.0f * u * t * t) +
            aP3 * (t * t * t);
        if (s_segmentIntersectsRect(prev, pt, aMin, aMax)) return true;
        prev = pt;
    }
    return false;
}

}  // namespace

// -----------------------------
// Graph
// -----------------------------
IMNODAL_API bool BeginGraph(Id aGraphId, const GraphSettings& arSettings) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(!rCtx.graphActive && "BeginGraph called twice without EndGraph");
    IM_ASSERT(aGraphId != 0 && "Graph id must be non-zero");

    rCtx.graphActive = true;
    rCtx.currentGraphId = aGraphId;

    // Scope ID under the graph so multiple graphs in the same canvas don't
    // see each other's widget IDs.
    ImGui::PushID(s_imguiId(aGraphId));

    GraphState& rGraph = rCtx.graphs[aGraphId];
    rGraph.id = aGraphId;
    rGraph.settings = arSettings;
    rGraph.frameNodeOrder.clear();
    rGraph.frameSlotOrder.clear();

    // Refresh link "selected" flags from this graph's selection (per-frame).
    for (auto& kv : rCtx.links) {
        kv.second.selected = (rGraph.selectedLinks.count(kv.first) > 0);
    }

    // Open multi-channel splitter for this graph. Default channel = Content.
    auto* const pDrawList = rCtx.drawList;
    rGraph.preBeginChannel = pDrawList->_Splitter._Current;
    pDrawList->ChannelsSplit(GC_Count);
    pDrawList->ChannelsSetCurrent(GC_Content);
    rGraph.splitterActive = true;

    return true;
}

IMNODAL_API void EndGraph() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.graphActive && "EndGraph without matching BeginGraph");
    IM_ASSERT(rCtx.currentNodeId == 0 && "EndGraph while a node is still open");
    IM_ASSERT(rCtx.currentSection == Context::Section_None && "EndGraph while a section is still open");

    GraphState& rGraph = rCtx.graphs[rCtx.currentGraphId];
    auto* const pDrawList = rCtx.drawList;

    // Background click = left click that nothing (node / link / slot-drag)
    // consumed this frame AND that actually lands inside the canvas widget.
    // Without the canvas-hover check a click on any other ImGui window (Params,
    // Debug, main menu…) would deselect, which is wrong: selection must only
    // clear when the user clicks an empty spot of the canvas itself (clicking
    // another node re-selects it via the BeginNode path).
    // NOTE: we're still inside local-space (io.MousePos has been remapped to
    // canvas coords by s_enterLocalSpace). widgetRect is in screen space, so
    // the hit-test must compare against the original screen-space mouse pos
    // saved in mousePosBackup — not io.MousePos / IsMouseHoveringRect.
    // ---- Box-select / bg-click state machine ----
    // mousePosBackup is screen-space (used for canvas hover); io.MousePos is
    // local-space inside BeginCanvas — we use the local mouse for everything
    // related to node/link rects so it scales with the zoom.
    const bool lmbClicked  = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool lmbDown     = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool lmbReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    const bool lmbOnCanvas = ImGui::IsWindowHovered() && rCtx.widgetRect.Contains(rCtx.mousePosBackup);
    const ImVec2 mouseLocal = ImGui::GetIO().MousePos;

    // Phase 1: arm a pending bg-click on empty-canvas mouse-down. On stocke
    // la position en LOCAL-SPACE (pour le rendu de la box) ET en SCREEN-SPACE
    // (pour le seuil de drag — immune aux pan/zoom du canvas qui peuvent
    // intervenir entre le clic et le check, par exemple si le double-clic
    // declenche un fit-to-view : le canvas-space mouse "saute" alors que la
    // souris ecran n'a pas bouge ; on ne veut pas armer un box-select pour
    // ce mouvement parasite).
    if (lmbClicked && lmbOnCanvas && !rGraph.clickConsumedThisFrame && !rCtx.isPanning) {
        rCtx.pendingBgClick = true;
        rCtx.pendingBgClickPos = mouseLocal;
        rCtx.pendingBgClickPosScreen = rCtx.mousePosBackup;
    }

    // Phase 2: promote pending click to box-select once drag distance crosses
    // the threshold. On compare en SCREEN-SPACE (mousePosBackup vs la position
    // screen au clic) ; le seuil est en pixels ecran sans dependre du zoom.
    if (rCtx.pendingBgClick && lmbDown && !rCtx.boxSelectActive && rGraph.settings.allowBoxSelect) {
        const ImVec2 d = rCtx.mousePosBackup - rCtx.pendingBgClickPosScreen;
        constexpr float kThreshold = 4.0f;  // screen-pixels
        if ((d.x * d.x + d.y * d.y) > (kThreshold * kThreshold)) {
            rCtx.boxSelectActive  = true;
            rCtx.boxSelectGraphId = rGraph.id;
            // Le start de la box doit etre en CANVAS-SPACE actuel : on
            // re-projette la position screen au clic via la transform
            // courante (qui peut avoir change si le canvas a zoome entre
            // temps — sinon equivalent a pendingBgClickPos).
            rCtx.boxSelectStart = (rCtx.pendingBgClickPosScreen - rCtx.viewTransformPos) * rCtx.invScale;
        }
    }

    // Phase 3: while box-select is active, draw the rectangle on the overlay channel.
    if (rCtx.boxSelectActive && rCtx.boxSelectGraphId == rGraph.id) {
        const ImVec2 mn(ImMin(rCtx.boxSelectStart.x, mouseLocal.x), ImMin(rCtx.boxSelectStart.y, mouseLocal.y));
        const ImVec2 mx(ImMax(rCtx.boxSelectStart.x, mouseLocal.x), ImMax(rCtx.boxSelectStart.y, mouseLocal.y));
        s_setChannel(rCtx, GC_Overlay);
        rCtx.drawList->AddRectFilled(mn, mx, IM_COL32(120, 200, 255, 40));
        rCtx.drawList->AddRect(mn, mx, IM_COL32(120, 200, 255, 200));
        s_setChannel(rCtx, GC_Content);
    }

    // Phase 4: on release — either commit the box (if it was a drag) or treat
    // as a plain bg-click and clear the selection.
    if (lmbReleased && rCtx.pendingBgClick) {
        if (rCtx.boxSelectActive && rCtx.boxSelectGraphId == rGraph.id) {
            const ImVec2 mn(ImMin(rCtx.boxSelectStart.x, mouseLocal.x), ImMin(rCtx.boxSelectStart.y, mouseLocal.y));
            const ImVec2 mx(ImMax(rCtx.boxSelectStart.x, mouseLocal.x), ImMax(rCtx.boxSelectStart.y, mouseLocal.y));
            const bool toggle = s_multiSelectHeld(rGraph);
            if (!toggle) s_clearSelection(rGraph);

            // Nodes : selectionnes des que leur rect VISUEL intersecte le box
            // (overlap AABB-AABB). Plus tolerant que "centre dans le box" :
            // un coin du node qui touche le box suffit.
            for (const auto& kv : rCtx.nodes) {
                if (kv.second.graphId != rGraph.id) continue;
                const ImRect& r = kv.second.lastScreenRect;
                if (r.Min.x >= r.Max.x) continue;
                const bool overlaps =
                    r.Min.x <= mx.x && r.Max.x >= mn.x &&
                    r.Min.y <= mx.y && r.Max.y >= mn.y;
                if (overlaps) {
                    if (rGraph.selectedNodes.insert(kv.first).second) {
                        rGraph.lastSelectedNode = kv.first;
                        rGraph.selectionChangedThisFrame = true;
                    }
                }
            }
            // Links : selectionnes des que la spline (le bezier cache) touche
            // le box, meme partiellement. On sample en chords et on teste
            // chord-vs-rect — un seul "frolement" suffit.
            for (const auto& kv : rCtx.links) {
                if (kv.second.graphId != rGraph.id) continue;
                if (!kv.second.bezierCached) continue;
                if (s_bezierIntersectsRect(
                        kv.second.cachedFromPos, kv.second.cachedP1,
                        kv.second.cachedP2,     kv.second.cachedToPos,
                        mn, mx)) {
                    if (rGraph.selectedLinks.insert(kv.first).second) {
                        rGraph.lastSelectedLink = kv.first;
                        rGraph.selectionChangedThisFrame = true;
                    }
                }
            }
            rCtx.boxSelectActive  = false;
            rCtx.boxSelectGraphId = 0;
        } else {
            // Plain bg-click → clear selection unless multi-modifier is held.
            if (!s_multiSelectHeld(rGraph)) {
                s_clearSelection(rGraph);
            }
        }
        rCtx.pendingBgClick    = false;
        rCtx.pendingBgClickPos = ImVec2(0.0f, 0.0f);
    }
    // Reset for next frame
    rGraph.clickConsumedThisFrame = false;

    // ---- Deferred slot dot draw ----
    // EndSlot stashed slot ids here; Link() updated their connected flag
    // since then. NOW we know which color to use for each dot. We also draw
    // a button-style "hover frame" around the slot's hit rect when hovered —
    // gives the user a clear visual that the slot is interactive (just like
    // an ImGui::Button highlights on hover).
    if (!rGraph.frameSlotOrder.empty()) {
        pDrawList->ChannelsSetCurrent(GC_Background);
        constexpr float kHoverRounding = 3.0f;
        const ImU32 kHoverFill   = IM_COL32(255, 255, 255, 32);
        const ImU32 kHoverBorder = IM_COL32(255, 255, 255, 110);
        for (Id sid : rGraph.frameSlotOrder) {
            auto it = rCtx.slots.find(sid);
            if (it == rCtx.slots.end()) continue;
            const SlotState& s = it->second;
            // Pour un slot dont le node parent est un reroute : pas de halo
            // de hover etendu — il prendrait la place de la frame de selection
            // (qui est un peu plus grande que le dot) et le user ne pourrait
            // plus cliquer DANS la zone du frame pour selectionner. On laisse
            // juste le dot changer de couleur via dotColorHovered (le seul
            // retour visuel sur reroute, comme dans Unreal).
            bool isOnReroute = false;
            if (s.parentNode != 0) {
                auto nIt = rCtx.nodes.find(s.parentNode);
                if (nIt != rCtx.nodes.end()) isOnReroute = nIt->second.isReroute;
            }
            if (s.hovered && !isOnReroute && s.lastHitRect.Min.x < s.lastHitRect.Max.x) {
                pDrawList->AddRectFilled(s.lastHitRect.Min, s.lastHitRect.Max, kHoverFill, kHoverRounding);
                pDrawList->AddRect(s.lastHitRect.Min, s.lastHitRect.Max, kHoverBorder, kHoverRounding, 0, 1.0f);
            }
            // Dot on top.
            const ImU32 col = s.hovered   ? s.dotColorHovered
                            : s.connected ? s.dotColorConnected
                            :               s.dotColor;
            pDrawList->AddCircleFilled(s.screenPos, s.dotRadius, col);
        }
        pDrawList->ChannelsSetCurrent(GC_Content);
    }

    // Merge channels back into the canvas-expected channel order.
    if (rGraph.splitterActive) {
        pDrawList->ChannelsMerge();
        rGraph.splitterActive = false;
    }
    // Splitter must be back to the canvas's expected channel
    IM_ASSERT(pDrawList->_Splitter._Current == rCtx.expectedChannel && "EndGraph: splitter not restored to canvas expected channel");

    // Match the PushID at BeginGraph.
    ImGui::PopID();

    rCtx.graphActive = false;
    rCtx.currentGraphId = 0;
}

IMNODAL_API Id GetCurrentGraphId() {
    return s_getCtx().currentGraphId;
}

// -----------------------------
// Node
// -----------------------------
IMNODAL_API bool BeginNode(Id aNodeId, ImVec2* apPos, const NodeSettings& arSettings) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.graphActive && "BeginNode must be called inside BeginGraph/EndGraph");
    IM_ASSERT(aNodeId != 0 && "Node id must be non-zero");

    NodeState& rNode = rCtx.nodes[aNodeId];
    rNode.graphId = rCtx.currentGraphId;
    rNode.settings = arSettings;
    rNode.hasHeader = false;
    rNode.bodyColumnOpened = false;
    rNode.isReroute = false;        // BeginRerouteNode re-set this AFTER BeginNode if needed
    rNode.rerouteSlotId = 0;
    rNode.userPosPtr = apPos;

    // Sync position from user's storage
    if (apPos != nullptr) {
        rNode.pos = *apPos;
    }

    rCtx.currentNodeId = aNodeId;
    rCtx.graphs[rCtx.currentGraphId].frameNodeOrder.push_back(aNodeId);

    // Place ImGui cursor at node's canvas-space position, scoped by a unique ID.
    ImGui::PushID(s_imguiId(aNodeId));
    ImGui::SetCursorScreenPos(rNode.pos);  // canvas space = local space under canvas transform

    // Wrap the whole node in a group so we can read its rect at EndNode.
    ImGui::BeginGroup();

    // We stay on GC_Content channel — user widgets draw here.
    s_setChannel(rCtx, GC_Content);

    return true;
}

IMNODAL_API void EndNode() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.currentNodeId != 0 && "EndNode without matching BeginNode");
    IM_ASSERT(rCtx.currentSection == Context::Section_None && "EndNode while a section is still open");

    NodeState& rNode = rCtx.nodes[rCtx.currentNodeId];
    GraphState& rGraph = rCtx.graphs[rCtx.currentGraphId];

    ImGui::EndGroup();
    const ImVec2 contentMin = ImGui::GetItemRectMin();
    const ImVec2 contentMax = ImGui::GetItemRectMax();

    // Visual rect = content rect expanded by bodyPadding on each side. This
    // is what the user sees as the node's border, so slot dots (which sit
    // at the content-edge) end up `bodyPadding` away from the visible border.
    const float pad = rNode.settings.bodyPadding;
    const ImVec2 nodeMin(contentMin.x - pad, contentMin.y - pad);
    const ImVec2 nodeMax(contentMax.x + pad, contentMax.y + pad);
    rNode.size = nodeMax - nodeMin;

    // Hover test : rect classique sauf pour les reroutes qui sont circulaires
    // -> distance au centre du dot, rayon legerement superieur a la zone visuelle.
    if (rNode.isReroute) {
        auto sIt = rCtx.slots.find(rNode.rerouteSlotId);
        const ImVec2 center = (sIt != rCtx.slots.end())
            ? sIt->second.screenPos
            : ImVec2((nodeMin.x + nodeMax.x) * 0.5f, (nodeMin.y + nodeMax.y) * 0.5f);
        const float dotR  = (sIt != rCtx.slots.end()) ? sIt->second.dotRadius : 5.0f;
        const float hitR  = dotR + 6.0f;
        const ImVec2 mp   = ImGui::GetIO().MousePos;
        const float dx = mp.x - center.x;
        const float dy = mp.y - center.y;
        rNode.hovered = (dx * dx + dy * dy <= hitR * hitR) && ImGui::IsWindowHovered();
    } else {
        rNode.hovered = ImGui::IsMouseHoveringRect(nodeMin, nodeMax) && ImGui::IsWindowHovered();
    }
    if (rNode.hovered) {
        rCtx.currentHoveredNode = rCtx.currentNodeId;
    }

    // ---- Selection + drag (left-click-hold on the node rect) ----
    const bool lmbClicked   = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool lmbDown      = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool lmbReleased  = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    const bool rmbClicked   = ImGui::IsMouseClicked(rCtx.settings.contextMenuButton);
    // "topmost" = mouse on node AND no user widget is above the cursor.
    const bool hoveredTopMost = rNode.hovered && !ImGui::IsAnyItemHovered();

    if (hoveredTopMost && lmbClicked) {
        const bool toggle = s_multiSelectHeld(rGraph);
        s_selectNode(rGraph, rCtx.currentNodeId, toggle);
        rGraph.clickConsumedThisFrame = true;
        // Don't start a drag when toggling — the user is building a selection,
        // not moving the node.
        if (rNode.settings.movable && !toggle) {
            rCtx.draggingNodeId = rCtx.currentNodeId;
            rCtx.dragStartNodePos = rNode.pos;
            rCtx.dragStartMouseCanvas = ImGui::GetIO().MousePos;  // canvas space (we're in local space)
        }
    }
    if (rCtx.draggingNodeId == rCtx.currentNodeId) {
        if (lmbDown) {
            const ImVec2 delta = ImGui::GetIO().MousePos - rCtx.dragStartMouseCanvas;
            rNode.pos = rCtx.dragStartNodePos + delta;
            rNode.dragging = true;
        }
        if (lmbReleased) {
            rCtx.draggingNodeId = 0;
            rNode.dragging = false;
        }
    } else {
        rNode.dragging = false;
    }
    rNode.selected = (rGraph.selectedNodes.count(rCtx.currentNodeId) > 0);

    // Right-click on the node rect (and not on a widget above) → context menu request.
    if (hoveredTopMost && rmbClicked) {
        rNode.ctxMenuRequested = true;
        rCtx.ctxMenuNodeId = rCtx.currentNodeId;
    }

    // Write drag result back to the user's master copy
    if (rNode.userPosPtr != nullptr) {
        *rNode.userPosPtr = rNode.pos;
    }

    // ---- Draw background + border on GC_Background (under content) ----
    auto* const pDrawList = rCtx.drawList;
    s_setChannel(rCtx, GC_Background);

    const float rounding = rNode.settings.rounding;
    // Body fill
    pDrawList->AddRectFilled(nodeMin, nodeMax, rNode.settings.bodyColor, rounding);
    // Header tint (if header was emitted)
    if (rNode.hasHeader) {
        ImRect hdr = rNode.headerScreenRect;
        // Extend header visual to the full visual node width and up to the
        // visible top edge (so the band touches the rounded corner).
        hdr.Min.x = nodeMin.x;
        hdr.Min.y = nodeMin.y;
        hdr.Max.x = nodeMax.x;
        pDrawList->AddRectFilled(hdr.Min, hdr.Max, rNode.settings.headerColor, rounding, ImDrawFlags_RoundCornersTop);
    }
    // Border (selected color overrides)
    const ImU32 borderCol = rNode.selected ? rNode.settings.selectedBorderColor : rNode.settings.borderColor;
    if (rNode.isReroute) {
        // Frame circulaire au centre du dot. Toujours dessinee (alpha de
        // borderColor par defaut deja faible -> faint frame en permanence ;
        // selectedBorderColor plus opaque -> highlight a la selection).
        auto sIt = rCtx.slots.find(rNode.rerouteSlotId);
        const ImVec2 center = (sIt != rCtx.slots.end())
            ? sIt->second.screenPos
            : ImVec2((nodeMin.x + nodeMax.x) * 0.5f, (nodeMin.y + nodeMax.y) * 0.5f);
        const float dotR  = (sIt != rCtx.slots.end()) ? sIt->second.dotRadius : 5.0f;
        const float ringR = dotR + 6.0f;
        pDrawList->AddCircle(center, ringR, borderCol, 0, rNode.settings.borderThickness);
    } else {
        pDrawList->AddRect(nodeMin, nodeMax, borderCol, rounding, 0, rNode.settings.borderThickness);
    }

    // Hover-only drag handle bar — shown only when mouse is on the node.
    // Used by reroute-style nodes that have no header: gives the user a visible
    // grab surface to drag the node, appearing only when needed.
    if (rNode.settings.drawHoverHandle && rNode.hovered) {
        const float h = rNode.settings.hoverHandleHeight;
        const ImVec2 bMin(nodeMin.x + 2.0f, nodeMin.y + 1.0f);
        const ImVec2 bMax(nodeMax.x - 2.0f, nodeMin.y + 1.0f + h);
        pDrawList->AddRectFilled(bMin, bMax, rNode.settings.hoverHandleColor, h * 0.5f);
    }

    // Restore content channel for next widgets
    s_setChannel(rCtx, GC_Content);

    // Cache the visual rect (local-space) for GetNodeRect / per-node drawlists.
    rNode.lastScreenRect = ImRect(nodeMin, nodeMax);

    ImGui::PopID();

    rCtx.currentNodeId = 0;
}

// -----------------------------
// Sections
// -----------------------------
IMNODAL_API bool BeginHeader() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.currentNodeId != 0 && "BeginHeader outside of BeginNode/EndNode");
    IM_ASSERT(rCtx.currentSection == Context::Section_None && "BeginHeader while another section is still open");
    rCtx.currentSection = Context::Section_Header;
    ImGui::PushID("##imnodal_header");
    ImGui::BeginGroup();
    return true;
}
IMNODAL_API void EndHeader() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.currentSection == Context::Section_Header && "EndHeader without matching BeginHeader");
    ImGui::EndGroup();
    NodeState& rNode = rCtx.nodes[rCtx.currentNodeId];
    rNode.hasHeader = true;
    rNode.headerScreenRect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::PopID();
    rCtx.currentSection = Context::Section_None;
}

// Helper: open body column — if another body column was already opened, emit SameLine first.
static bool s_beginBodyColumn(Context& rCtx, Context::Section aSec, const char* aIdLabel) {
    IM_ASSERT(rCtx.currentNodeId != 0 && "BeginInputs/Outputs/Center outside of BeginNode/EndNode");
    IM_ASSERT(rCtx.currentSection == Context::Section_None && "BeginInputs/Outputs/Center while another section is still open");
    NodeState& rNode = rCtx.nodes[rCtx.currentNodeId];
    if (rNode.bodyColumnOpened) {
        ImGui::SameLine(0.0f, rNode.settings.columnSpacing);
    }
    rCtx.currentSection = aSec;
    ImGui::PushID(aIdLabel);
    ImGui::BeginGroup();
    return true;
}
static void s_endBodyColumn(Context& rCtx, Context::Section aSec) {
    IM_ASSERT(rCtx.currentSection == aSec && "EndInputs/Outputs/Center without matching Begin");
    ImGui::EndGroup();
    ImGui::PopID();
    rCtx.nodes[rCtx.currentNodeId].bodyColumnOpened = true;
    rCtx.currentSection = Context::Section_None;
}

IMNODAL_API bool BeginInputs()  { return s_beginBodyColumn(s_getCtx(), Context::Section_Inputs,  "##imnodal_inputs"); }
IMNODAL_API void EndInputs()    { s_endBodyColumn(s_getCtx(), Context::Section_Inputs); }
IMNODAL_API bool BeginCenter()  { return s_beginBodyColumn(s_getCtx(), Context::Section_Center,  "##imnodal_center"); }
IMNODAL_API void EndCenter()    { s_endBodyColumn(s_getCtx(), Context::Section_Center); }
IMNODAL_API bool BeginOutputs() { return s_beginBodyColumn(s_getCtx(), Context::Section_Outputs, "##imnodal_outputs"); }
IMNODAL_API void EndOutputs()   { s_endBodyColumn(s_getCtx(), Context::Section_Outputs); }

IMNODAL_API bool BeginFooter() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.currentNodeId != 0 && "BeginFooter outside of BeginNode/EndNode");
    IM_ASSERT(rCtx.currentSection == Context::Section_None && "BeginFooter while another section is still open");
    rCtx.currentSection = Context::Section_Footer;
    ImGui::PushID("##imnodal_footer");
    ImGui::BeginGroup();
    return true;
}
IMNODAL_API void EndFooter() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.currentSection == Context::Section_Footer && "EndFooter without matching BeginFooter");
    ImGui::EndGroup();
    ImGui::PopID();
    rCtx.currentSection = Context::Section_None;
}

// BeginAlign / EndAlign — layout container that horizontally aligns the
// widgets emitted between them. Works in immediate mode by remembering the
// group width measured on the previous frame and using it to compute this
// frame's indent. The widgets themselves are real ImGui widgets — full
// styling, hover and click semantics behave exactly as outside.
IMNODAL_API void BeginAlign(float aRatio, float aAvailableWidth) {
    Context& rCtx = s_getCtx();

    // Stable per-call ID so the same BeginAlign call site finds its own
    // measurement frame after frame even when there are several in a row.
    rCtx.alignCallCounter++;
    char buf[32];
    ImFormatString(buf, sizeof(buf), "##imnodal_align_%d", rCtx.alignCallCounter);
    ImGuiWindow* pWindow = ImGui::GetCurrentWindow();
    const ImGuiID id = pWindow->GetID(buf);
    rCtx.alignStack.push_back(id);

    AlignSlot& rSlot = rCtx.alignSlots[id];

    // Resolve the alignment width.
    //
    // Inside a node we want the CONTENT width — that is, the area in which
    // user widgets actually live, NOT the visual rect (which extends outward
    // by `bodyPadding` on each side). Reasons:
    //   1) The cursor at BeginAlign sits at contentMin.x, and the visual rect
    //      is symmetric around content (pad on each side). So content center
    //      == visual center: centering against contentWidth at this cursor
    //      lands the widget at the visual center too.
    //   2) Using visualWidth instead would shift the widget right by `pad` for
    //      ratio 0.5, and worse: when the alignment is the only contributor
    //      to the node width (header-only node), the indent feeds back into
    //      next frame's measured width and the node grows every frame until
    //      it converges to a wrong steady state.
    float availW = aAvailableWidth;
    if (availW <= 0.0f) {
        if (rCtx.currentNodeId != 0) {
            auto it = rCtx.nodes.find(rCtx.currentNodeId);
            if (it != rCtx.nodes.end() && it->second.size.x > 0.0f) {
                availW = it->second.size.x - 2.0f * it->second.settings.bodyPadding;
                if (availW < 0.0f) availW = 0.0f;
            }
        }
        if (availW <= 0.0f) availW = ImGui::GetContentRegionAvail().x;
    }

    // First frame on this scope: lastWidth is 0 → no indent (left-aligned
    // until next frame measures the actual content). Steady state: indent
    // shifts the group so it lands at the requested ratio.
    float indent = 0.0f;
    if (rSlot.lastWidth > 0.0f && availW > rSlot.lastWidth) {
        indent = aRatio * (availW - rSlot.lastWidth);
    }
    rSlot.appliedIndent = indent;
    if (indent > 0.0f) ImGui::Indent(indent);

    // Push an ID so user widgets emitted inside the alignment scope don't
    // collide with widgets sharing the same labels in sibling scopes.
    ImGui::PushID(buf);
    // Group everything user emits so we can read its bbox at EndAlign.
    ImGui::BeginGroup();
}

IMNODAL_API void EndAlign() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(!rCtx.alignStack.empty() && "EndAlign without matching BeginAlign");
    const ImGuiID id = rCtx.alignStack.back();
    rCtx.alignStack.pop_back();

    ImGui::EndGroup();
    ImGui::PopID();
    AlignSlot& rSlot = rCtx.alignSlots[id];
    rSlot.lastWidth = ImGui::GetItemRectSize().x;
    if (rSlot.appliedIndent > 0.0f) ImGui::Unindent(rSlot.appliedIndent);
}

// -----------------------------
// Slot primitive
// -----------------------------
IMNODAL_API bool BeginSlot(Id aSlotId, SlotRole aRole, const char* aLabel, const SlotSettings& arSettings) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(aSlotId != 0 && "Slot id must be non-zero");
    IM_ASSERT(rCtx.currentSlotId == 0 && "Nested BeginSlot is not supported");
    SlotState& rSlot = rCtx.slots[aSlotId];
    rSlot.parentNode        = rCtx.currentNodeId;
    rSlot.graphId           = rCtx.currentGraphId;
    rSlot.role              = aRole;
    rSlot.typeTag           = arSettings.typeTag;
    rSlot.dotRadius         = arSettings.dotRadius;
    rSlot.dotColor          = arSettings.dotColor;
    rSlot.dotColorConnected = arSettings.dotColorConnected;
    rSlot.dotColorHovered   = arSettings.dotColorHovered;

    // EARLY hover hit-test against the previous frame's hit rect. Without this,
    // rSlot.hovered would stay false until EndSlot — but the host typically
    // queries IsSlotHovered() / double-click handlers BETWEEN BeginSlot and
    // EndSlot, so they'd always read false. Using last frame's rect introduces
    // a 1-frame lag that's invisible for stable layouts (which is the common
    // case). EndSlot will refine the value with this frame's actual rect.
    {
        const ImRect& r = rSlot.lastHitRect;
        if (r.Min.x < r.Max.x && r.Min.y < r.Max.y) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            rSlot.hovered = (mp.x >= r.Min.x && mp.x <= r.Max.x &&
                             mp.y >= r.Min.y && mp.y <= r.Max.y);
        } else {
            rSlot.hovered = false;
        }
    }

    rCtx.currentSlotId = aSlotId;

    ImGui::PushID(s_imguiId(aSlotId));
    ImGui::BeginGroup();

    // Inline layout — dot sits next to the label/widget based on role:
    //   Input  → dot first, then label/widget
    //   Output → label/widget first, dot appended in EndSlot
    //   InOut  → label/widget first; dot drawn centered on the group at EndSlot
    const float padding = arSettings.dotRadius * 2.0f + 4.0f;
    const bool  isOutput = (aRole == SlotRole_Output);
    const bool  isInOut  = (aRole == SlotRole_InOut);

    if (!isOutput && !isInOut) {
        ImGui::Dummy(ImVec2(padding, 0.0f));
        ImGui::SameLine(0.0f, 0.0f);
    }
    if (aLabel && aLabel[0] != 0) {
        ImGui::TextUnformatted(aLabel);
        ImGui::SameLine();
    }
    return true;
}

IMNODAL_API void EndSlot() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.currentSlotId != 0 && "EndSlot without matching BeginSlot");

    SlotState& rSlot = rCtx.slots[rCtx.currentSlotId];

    const bool isOutput = (rSlot.role == SlotRole_Output);
    const bool isInOut  = (rSlot.role == SlotRole_InOut);
    const float padding = rSlot.dotRadius * 2.0f + 4.0f;

    // Output / InOut: append tail padding so the dot has room on the right.
    if (isOutput || isInOut) {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Dummy(ImVec2(padding, 0.0f));
    }

    ImGui::EndGroup();
    const ImVec2 gMin = ImGui::GetItemRectMin();
    const ImVec2 gMax = ImGui::GetItemRectMax();
    const float yMid  = (gMin.y + gMax.y) * 0.5f;

    // Pivot override (set via SlotDotPivotAlignment / SlotDotPivotOffset between
    // BeginSlot and EndSlot) — replaces the role-based default. Tangent is
    // auto-derived from the alignment so the link curves leave the slot toward
    // the dominant side of the override.
    const bool pivotOverride = (rCtx.slotPivotAlignment.x >= 0.0f && rCtx.slotPivotAlignment.y >= 0.0f);
    if (pivotOverride) {
        const ImVec2 size = gMax - gMin;
        rSlot.screenPos = gMin + size * rCtx.slotPivotAlignment + rCtx.slotPivotOffset;
        const float dx = rCtx.slotPivotAlignment.x - 0.5f;
        const float dy = rCtx.slotPivotAlignment.y - 0.5f;
        if (std::fabs(dx) >= std::fabs(dy)) {
            rSlot.tangent = ImVec2(dx >= 0.0f ? 1.0f : -1.0f, 0.0f);
        } else {
            rSlot.tangent = ImVec2(0.0f, dy >= 0.0f ? 1.0f : -1.0f);
        }
    } else if (isInOut) {
        // Centered on the group rect; tangent sentinel (0,0) → resolved
        // dynamically at link draw time from the other endpoint direction.
        const float xMid = (gMin.x + gMax.x) * 0.5f;
        rSlot.screenPos = ImVec2(xMid, yMid);
        rSlot.tangent   = ImVec2(0.0f, 0.0f);
    } else if (isOutput) {
        rSlot.screenPos = ImVec2(gMax.x - rSlot.dotRadius, yMid);
        rSlot.tangent   = ImVec2(1.0f, 0.0f);
    } else {
        rSlot.screenPos = ImVec2(gMin.x + rSlot.dotRadius, yMid);
        rSlot.tangent   = ImVec2(-1.0f, 0.0f);
    }
    // Reset pivot override for the next slot.
    rCtx.slotPivotAlignment = ImVec2(-1.0f, -1.0f);
    rCtx.slotPivotOffset    = ImVec2(0.0f, 0.0f);

    // Hit area : par defaut tout le rect du groupe + un halo autour du dot.
    // EXCEPTION pour un slot de reroute : on restreint au DOT lui-meme. Sinon
    // la zone autour du dot serait happee par le slot (= drag de lien) et le
    // user ne pourrait plus cliquer "a cote" pour selectionner / bouger le
    // node. UE-style : hover sur le dot = lien, hover sur le reste = node.
    bool slotOnReroute = false;
    if (rCtx.currentNodeId != 0) {
        auto nIt = rCtx.nodes.find(rCtx.currentNodeId);
        if (nIt != rCtx.nodes.end()) slotOnReroute = nIt->second.isReroute;
    }
    ImRect hitBB;
    if (slotOnReroute) {
        const float r = rSlot.dotRadius;
        hitBB = ImRect(rSlot.screenPos - ImVec2(r, r),
                       rSlot.screenPos + ImVec2(r, r));
    } else {
        const float pad = rSlot.dotRadius + 4.0f;
        hitBB = ImRect(
            ImMin(gMin, rSlot.screenPos - ImVec2(pad, pad)),
            ImMax(gMax, rSlot.screenPos + ImVec2(pad, pad)));
    }
    // Cache for next frame's BeginSlot early hover test + EndGraph hover frame.
    rSlot.lastHitRect = hitBB;

    // Treat the slot as a single button-like widget regardless of where it
    // lives (inside a node section, inline in a window, ...). ButtonBehavior
    // handles hover/click/ActiveId; the host window won't move because the
    // slot owns ActiveId once held.
    ImGuiWindow* const pWindow = ImGui::GetCurrentWindow();
    const ImGuiID hitId = pWindow->GetID("##imnodal_slot_hit");
    ImGui::KeepAliveID(hitId);
    bool btnHovered = false, btnHeld = false;
    if (ImGui::ItemAdd(hitBB, hitId)) {
        ImGui::ButtonBehavior(hitBB, hitId, &btnHovered, &btnHeld,
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    }
    // Raw geometric hover in parallel: while dragging from slot A,
    // ButtonBehavior on slot B reports hovered=false (ActiveId gating).
    // The raw test bypasses that so the target dot still highlights and
    // currentHoveredSlot tracks correctly for link-target detection.
    const bool mouseOnSlot = ImGui::IsMouseHoveringRect(hitBB.Min, hitBB.Max, false);
    rSlot.hovered = btnHovered || mouseOnSlot;
    if (mouseOnSlot) {
        rCtx.currentHoveredSlot = rCtx.currentSlotId;
    }

    // Start a connection drag on click.
    if (btnHeld && rCtx.draggingFromSlot == 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        rCtx.draggingFromSlot = rCtx.currentSlotId;
        if (rCtx.graphActive) {
            rCtx.graphs[rCtx.currentGraphId].clickConsumedThisFrame = true;
        }
    }

    // Right-click on the slot → context menu request.
    if (mouseOnSlot && ImGui::IsMouseClicked(rCtx.settings.contextMenuButton)) {
        rSlot.ctxMenuRequested = true;
        rCtx.ctxMenuSlotId = rCtx.currentSlotId;
    }

    // Dot draw — inside a graph, defer to EndGraph so the connected state
    // (set by Link() calls happening AFTER nodes are emitted) is up-to-date.
    // Standalone slots draw immediately since there's no later phase.
    if (rCtx.graphActive) {
        rCtx.graphs[rCtx.currentGraphId].frameSlotOrder.push_back(rCtx.currentSlotId);
    } else {
        const ImU32 col = rSlot.hovered   ? rSlot.dotColorHovered
                        : rSlot.connected ? rSlot.dotColorConnected
                        :                   rSlot.dotColor;
        ImGui::GetWindowDrawList()->AddCircleFilled(rSlot.screenPos, rSlot.dotRadius, col);
    }

    ImGui::PopID();
    rCtx.currentSlotId = 0;
}

IMNODAL_API bool BeginInputSlot(Id aSlotId, const char* aLabel, const SlotSettings& arSettings) {
    return BeginSlot(aSlotId, SlotRole_Input, aLabel, arSettings);
}
IMNODAL_API bool BeginOutputSlot(Id aSlotId, const char* aLabel, const SlotSettings& arSettings) {
    return BeginSlot(aSlotId, SlotRole_Output, aLabel, arSettings);
}

// -----------------------------
// Queries
// -----------------------------
IMNODAL_API ImVec2 GetSlotScreenPos(Id aSlotId) {
    Context& rCtx = s_getCtx();
    auto it = rCtx.slots.find(aSlotId);
    return (it != rCtx.slots.end()) ? it->second.screenPos : ImVec2(0.0f, 0.0f);
}
IMNODAL_API ImVec2 GetSlotTangent(Id aSlotId) {
    Context& rCtx = s_getCtx();
    auto it = rCtx.slots.find(aSlotId);
    return (it != rCtx.slots.end()) ? it->second.tangent : ImVec2(1.0f, 0.0f);
}
IMNODAL_API bool IsSlotHovered(Id aSlotId) {
    Context& rCtx = s_getCtx();
    auto it = rCtx.slots.find(aSlotId);
    return (it != rCtx.slots.end()) && it->second.hovered;
}
IMNODAL_API bool IsSlotConnected(Id aSlotId) {
    Context& rCtx = s_getCtx();
    auto it = rCtx.slots.find(aSlotId);
    return (it != rCtx.slots.end()) && it->second.connected;
}

IMNODAL_API bool IsNodeHovered(Id* apoNodeId) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.graphActive && "IsNodeHovered must be called inside BeginGraph/EndGraph");
    GraphState& rGraph = rCtx.graphs[rCtx.currentGraphId];
    // Walk draw order reverse so top-most wins
    for (int i = (int)rGraph.frameNodeOrder.size() - 1; i >= 0; --i) {
        Id nid = rGraph.frameNodeOrder[i];
        auto it = rCtx.nodes.find(nid);
        if (it != rCtx.nodes.end() && it->second.hovered) {
            if (apoNodeId) *apoNodeId = nid;
            return true;
        }
    }
    return false;
}
IMNODAL_API bool IsNodeSelected(Id aNodeId) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.graphActive && "IsNodeSelected must be called inside BeginGraph/EndGraph");
    return rCtx.graphs[rCtx.currentGraphId].selectedNodes.count(aNodeId) > 0;
}
IMNODAL_API Id GetSelectedNode() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.graphActive && "GetSelectedNode must be called inside BeginGraph/EndGraph");
    GraphState& g = rCtx.graphs[rCtx.currentGraphId];
    if (g.lastSelectedNode != 0 && g.selectedNodes.count(g.lastSelectedNode) > 0) {
        return g.lastSelectedNode;
    }
    return g.selectedNodes.empty() ? (Id)0 : *g.selectedNodes.begin();
}
IMNODAL_API void SetSelectedNode(Id aNodeId) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.graphActive && "SetSelectedNode must be called inside BeginGraph/EndGraph");
    GraphState& g = rCtx.graphs[rCtx.currentGraphId];
    if (aNodeId == 0) {
        s_clearSelection(g);
    } else {
        s_selectNode(g, aNodeId, false);
    }
}
IMNODAL_API bool IsNodeDragging(Id aNodeId) {
    Context& rCtx = s_getCtx();
    return rCtx.draggingNodeId == aNodeId;
}

IMNODAL_API bool IsLinkHovered(Id aLinkId) {
    Context& rCtx = s_getCtx();
    auto it = rCtx.links.find(aLinkId);
    return (it != rCtx.links.end()) && it->second.hovered;
}
IMNODAL_API bool IsLinkClicked(Id aLinkId, int aButton) {
    (void)aButton;  // M2 only tracks left click
    Context& rCtx = s_getCtx();
    auto it = rCtx.links.find(aLinkId);
    return (it != rCtx.links.end()) && it->second.clicked;
}
IMNODAL_API bool IsLinkDoubleClicked(Id aLinkId) {
    Context& rCtx = s_getCtx();
    auto it = rCtx.links.find(aLinkId);
    return (it != rCtx.links.end()) && it->second.doubleClicked;
}
IMNODAL_API bool IsLinkSelected(Id aLinkId) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.graphActive && "IsLinkSelected must be called inside BeginGraph/EndGraph");
    return rCtx.graphs[rCtx.currentGraphId].selectedLinks.count(aLinkId) > 0;
}
IMNODAL_API Id GetSelectedLink() {
    Context& rCtx = s_getCtx();
    if (rCtx.graphActive) {
        GraphState& g = rCtx.graphs[rCtx.currentGraphId];
        if (g.lastSelectedLink != 0 && g.selectedLinks.count(g.lastSelectedLink) > 0) {
            return g.lastSelectedLink;
        }
        return g.selectedLinks.empty() ? (Id)0 : *g.selectedLinks.begin();
    }
    return rCtx.standaloneSelectedLink;
}
IMNODAL_API void SetSelectedLink(Id aLinkId) {
    Context& rCtx = s_getCtx();
    if (rCtx.graphActive) {
        GraphState& g = rCtx.graphs[rCtx.currentGraphId];
        if (aLinkId == 0) {
            s_clearSelection(g);
        } else {
            s_selectLink(g, aLinkId, false);
        }
    } else {
        rCtx.standaloneSelectedLink = aLinkId;
    }
}
IMNODAL_API SlotRole GetSlotRole(Id aSlotId) {
    Context& rCtx = s_getCtx();
    auto it = rCtx.slots.find(aSlotId);
    return (it != rCtx.slots.end()) ? it->second.role : SlotRole_Input;
}

// =====================================================================
// Links (M2)
// =====================================================================

namespace {

// Cubic-Bezier control points from two anchor points + outward tangents.
// Control point distance scales with the separation so the curve "breathes"
// nicely when slots are far/close.
inline void s_bezierCtrl(const ImVec2& aFrom, const ImVec2& aFromTangent,
                         const ImVec2& aTo,   const ImVec2& aToTangent,
                         ImVec2& arP1, ImVec2& arP2) {
    const float dx = aTo.x - aFrom.x;
    const float dy = aTo.y - aFrom.y;
    float d = std::sqrt(dx * dx + dy * dy) * 0.4f;
    if (d < 30.0f) d = 30.0f;
    if (d > 200.0f) d = 200.0f;
    arP1 = aFrom + aFromTangent * d;
    arP2 = aTo   + aToTangent   * d;
}

// Resolve a slot's tangent. InOut slots use a sentinel (0,0) and get a tangent
// computed dynamically from the direction toward the other endpoint.
static ImVec2 s_resolveTangent(const SlotState& arSlot, const ImVec2& aOtherPos) {
    if (arSlot.tangent.x == 0.0f && arSlot.tangent.y == 0.0f) {
        // InOut sentinel : on adopte une tangente HORIZONTALE pure, signe
        // donne par la direction de l'autre extremite. C'est ce qui fait que
        // les splines passant par un reroute conservent l'allure des wires
        // standard (Input -> tangente -1,0 ; Output -> +1,0) au lieu de
        // pointer en biais quand le reroute est decale verticalement.
        const float dx = aOtherPos.x - arSlot.screenPos.x;
        return ImVec2(dx >= 0.0f ? 1.0f : -1.0f, 0.0f);
    }
    return arSlot.tangent;
}

static void s_drawBezierLink(ImDrawList* apDrawList,
                             const ImVec2& aFrom, const ImVec2& aFromTangent,
                             const ImVec2& aTo,   const ImVec2& aToTangent,
                             ImU32 aColor, float aThickness) {
    ImVec2 p1, p2;
    s_bezierCtrl(aFrom, aFromTangent, aTo, aToTangent, p1, p2);
    apDrawList->AddBezierCubic(aFrom, p1, p2, aTo, aColor, aThickness);
}

// Hit-test: is the mouse within `aThreshold` of the cubic Bézier curve?
// Approximated by sampling the curve into N line segments.
static bool s_isMouseOnBezier(const ImVec2& aP0, const ImVec2& aP1,
                              const ImVec2& aP2, const ImVec2& aP3,
                              const ImVec2& aMouse, float aThreshold) {
    const int kSegments = 24;
    ImVec2 prev = aP0;
    const float thr2 = aThreshold * aThreshold;
    for (int i = 1; i <= kSegments; ++i) {
        const float t = (float)i / (float)kSegments;
        const float u = 1.0f - t;
        const ImVec2 pt =
            aP0 * (u * u * u) +
            aP1 * (3.0f * u * u * t) +
            aP2 * (3.0f * u * t * t) +
            aP3 * (t * t * t);
        // Distance from aMouse to the segment (prev, pt).
        const ImVec2 seg = pt - prev;
        const ImVec2 toM = aMouse - prev;
        const float len2 = seg.x * seg.x + seg.y * seg.y;
        float u2 = 0.0f;
        if (len2 > 0.0f) {
            u2 = (toM.x * seg.x + toM.y * seg.y) / len2;
            if (u2 < 0.0f) u2 = 0.0f;
            else if (u2 > 1.0f) u2 = 1.0f;
        }
        const ImVec2 proj = prev + seg * u2;
        const ImVec2 delta = aMouse - proj;
        if (delta.x * delta.x + delta.y * delta.y <= thr2) return true;
        prev = pt;
    }
    return false;
}

}  // namespace

IMNODAL_API void Link(Id aLinkId, Id aFromSlotId, Id aToSlotId, ImU32 aColor, float aThickness) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(aLinkId != 0 && "Link id must be non-zero");

    auto itF = rCtx.slots.find(aFromSlotId);
    auto itT = rCtx.slots.find(aToSlotId);
    if (itF == rCtx.slots.end() || itT == rCtx.slots.end()) {
        return;
    }
    SlotState& rFrom = itF->second;
    SlotState& rTo   = itT->second;

    LinkState& rLink = rCtx.links[aLinkId];
    rLink.fromSlot  = aFromSlotId;
    rLink.toSlot    = aToSlotId;
    rLink.graphId   = rCtx.currentGraphId;
    rLink.thickness = aThickness;
    const ImU32 baseColor = (aColor != 0) ? aColor : IM_COL32(220, 220, 220, 230);
    rLink.color     = baseColor;

    rFrom.connected = true;
    rTo.connected   = true;

    // Resolve tangents (InOut slots are dynamic).
    const ImVec2 fromTan = s_resolveTangent(rFrom, rTo.screenPos);
    const ImVec2 toTan   = s_resolveTangent(rTo,   rFrom.screenPos);

    // Hit-test against the bezier.
    ImVec2 p1, p2;
    s_bezierCtrl(rFrom.screenPos, fromTan, rTo.screenPos, toTan, p1, p2);
    // Cache for FlowLink() to reuse this frame.
    rLink.cachedFromPos = rFrom.screenPos;
    rLink.cachedToPos   = rTo.screenPos;
    rLink.cachedP1      = p1;
    rLink.cachedP2      = p2;
    rLink.bezierCached  = true;

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float canvasScale = rCtx.scale > 0.0f ? rCtx.scale : 1.0f;
    const float hitThreshold = ImMax(aThickness * 2.0f, 6.0f / canvasScale);

    rLink.hovered = s_isMouseOnBezier(rFrom.screenPos, p1, p2, rTo.screenPos, mouse, hitThreshold);
    // NB : on NE reset PAS clicked / doubleClicked ici. NewFrame s'en charge
    // au debut de chaque frame. Si plusieurs Link() sont appeles avec le meme
    // segment id (= un segment partage entre plusieurs BaseLink, fusion
    // visuelle), le PREMIER appel detecte le clic et le set a true ; les
    // suivants ne re-entrent pas dans la branche canConsume (clickConsumed
    // est deja a true) et ecraseraient l'etat a false si on resetait ici.
    if (rLink.hovered) {
        rCtx.currentHoveredLink = aLinkId;
    }

    // Track selection at graph scope if we're inside a graph, otherwise use
    // the context-level standaloneSelectedLink field.
    GraphState* pGraph = rCtx.graphActive ? &rCtx.graphs[rCtx.currentGraphId] : nullptr;
    const bool clickConsumed = pGraph ? pGraph->clickConsumedThisFrame : false;

    const bool canConsume = rLink.hovered && !ImGui::IsAnyItemHovered() && !clickConsumed && rCtx.draggingFromSlot == 0;
    if (canConsume) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            rLink.clicked = true;
            if (pGraph) {
                const bool toggle = s_multiSelectHeld(*pGraph);
                s_selectLink(*pGraph, aLinkId, toggle);
                pGraph->clickConsumedThisFrame = true;
            } else {
                rCtx.standaloneSelectedLink = aLinkId;
            }
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            rLink.doubleClicked = true;
            if (pGraph) pGraph->clickConsumedThisFrame = true;
        }
        if (ImGui::IsMouseClicked(rCtx.settings.contextMenuButton)) {
            rLink.ctxMenuRequested = true;
            rCtx.ctxMenuLinkId = aLinkId;
        }
    }
    if (pGraph) {
        rLink.selected = (pGraph->selectedLinks.count(aLinkId) > 0);
    } else {
        rLink.selected = (rCtx.standaloneSelectedLink == aLinkId);
    }

    ImU32 drawColor = baseColor;
    if (rLink.selected) {
        drawColor = IM_COL32(255, 180, 0, 230);
    } else if (rLink.hovered) {
        drawColor = IM_COL32(255, 255, 255, 240);
    }

    // Draw on the Links channel when inside a graph (so it sits above node
    // content); otherwise draw directly on the current window's draw list.
    if (rCtx.graphActive) {
        auto* const pDrawList = rCtx.drawList;
        s_setChannel(rCtx, GC_Links);
        pDrawList->AddBezierCubic(rFrom.screenPos, p1, p2, rTo.screenPos, drawColor, aThickness);
        s_setChannel(rCtx, GC_Content);
    } else {
        ImGui::GetWindowDrawList()->AddBezierCubic(rFrom.screenPos, p1, p2, rTo.screenPos, drawColor, aThickness);
    }
}

// =====================================================================
// Connection creation state machine (M2)
// =====================================================================

IMNODAL_API bool BeginConnectionCreate() {
    Context& rCtx = s_getCtx();
    // True only when a drag is active AND it originated from the currently
    // open scope (graph or standalone). Without this check, a drag started in
    // one scope would trigger query/commit in every other scope too.
    return s_isDragInCurrentScope(rCtx);
}
IMNODAL_API bool QueryNewLink(Id* apoFromSlotId, Id* apoToSlotId) {
    Context& rCtx = s_getCtx();
    if (!s_isDragInCurrentScope(rCtx)) return false;
    const Id from = rCtx.draggingFromSlot;
    const Id to   = rCtx.currentHoveredSlot;
    if (to == 0 || to == from) return false;
    auto itF = rCtx.slots.find(from);
    auto itT = rCtx.slots.find(to);
    if (itF == rCtx.slots.end() || itT == rCtx.slots.end()) return false;
    // Target must live in the same scope too (no cross-scope links in M2).
    const Id currentScope = rCtx.graphActive ? rCtx.currentGraphId : (Id)0;
    if (itT->second.graphId != currentScope) return false;
    // Don't offer self-connections within the same node.
    if (itF->second.parentNode != 0 && itF->second.parentNode == itT->second.parentNode) {
        return false;
    }
    if (apoFromSlotId) *apoFromSlotId = from;
    if (apoToSlotId)   *apoToSlotId   = to;
    return true;
}

IMNODAL_API bool AcceptNewLink(ImU32 aColor) {
    Context& rCtx = s_getCtx();
    if (!s_isDragInCurrentScope(rCtx)) return false;
    rCtx.connAcceptedThisFrame = true;
    rCtx.connAcceptColor = (aColor != 0) ? aColor : IM_COL32(120, 255, 120, 230);
    rCtx.connRejectReason = nullptr;
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        rCtx.connCommitThisFrame = true;
        return true;
    }
    return false;
}

IMNODAL_API void RejectNewLink(const char* aReason) {
    Context& rCtx = s_getCtx();
    if (!s_isDragInCurrentScope(rCtx)) return;
    rCtx.connAcceptedThisFrame = false;
    rCtx.connAcceptColor = 0;
    rCtx.connRejectReason = aReason ? aReason : "invalid";
}

// True while the user is dragging a link AND no slot is under the cursor —
// i.e. they're hovering empty canvas. The host typically responds by opening
// a "create node connected to this slot" popup.
IMNODAL_API bool QueryNewNodeFromSlot(Id* apoFromSlotId) {
    Context& rCtx = s_getCtx();
    if (!s_isDragInCurrentScope(rCtx)) return false;
    if (rCtx.currentHoveredSlot != 0) return false;  // a target slot is hovered → that's QueryNewLink territory
    if (apoFromSlotId) *apoFromSlotId = rCtx.draggingFromSlot;
    return true;
}

IMNODAL_API bool AcceptNewNodeFromSlot(ImU32 aColor) {
    Context& rCtx = s_getCtx();
    if (!s_isDragInCurrentScope(rCtx)) return false;
    if (rCtx.currentHoveredSlot != 0) return false;
    rCtx.connNewNodeAcceptedThisFrame = true;
    rCtx.connAcceptColor = (aColor != 0) ? aColor : IM_COL32(120, 200, 255, 230);
    rCtx.connRejectReason = nullptr;
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        rCtx.connNewNodeCommitThisFrame = true;
        return true;
    }
    return false;
}

IMNODAL_API void EndConnectionCreate() {
    Context& rCtx = s_getCtx();

    // Only the scope that OWNS the drag draws the preview. Without this
    // check, every EndConnectionCreate call (graph + free-window + …) would
    // draw its own copy of the preview link.
    if (s_isDragInCurrentScope(rCtx)) {
        auto itF = rCtx.slots.find(rCtx.draggingFromSlot);
        if (itF != rCtx.slots.end()) {
            SlotState& rFrom = itF->second;

            // Skip the preview as long as the mouse is still inside the source
            // node's visual rect — without this the user sees an ugly tiny
            // preview from clicking the slot before they've actually started
            // dragging out of the node.
            bool insideSourceNode = false;
            if (rFrom.parentNode != 0) {
                auto nIt = rCtx.nodes.find(rFrom.parentNode);
                if (nIt != rCtx.nodes.end()) {
                    const ImRect& nr = nIt->second.lastScreenRect;
                    if (nr.Min.x < nr.Max.x) {
                        const ImVec2 mp = ImGui::GetIO().MousePos;
                        insideSourceNode = (mp.x >= nr.Min.x && mp.x <= nr.Max.x &&
                                            mp.y >= nr.Min.y && mp.y <= nr.Max.y);
                    }
                }
            }
            // We still draw the preview if the cursor is OVER another slot
            // (typical of "drag from one slot to another inside the same node",
            // unusual but possible) — that path uses the slot's known position
            // rather than the mouse, and the preview is informative there.
            const bool hoveringOtherSlot = (rCtx.currentHoveredSlot != 0 &&
                                            rCtx.currentHoveredSlot != rCtx.draggingFromSlot);

            if (!insideSourceNode || hoveringOtherSlot) {
                ImVec2 toPos;
                ImVec2 toTangent;
                if (hoveringOtherSlot) {
                    auto itT = rCtx.slots.find(rCtx.currentHoveredSlot);
                    if (itT != rCtx.slots.end()) {
                        toPos = itT->second.screenPos;
                        toTangent = s_resolveTangent(itT->second, rFrom.screenPos);
                    } else {
                        toPos = ImGui::GetIO().MousePos;
                        toTangent = -s_resolveTangent(rFrom, toPos);
                    }
                } else {
                    toPos = ImGui::GetIO().MousePos;
                    toTangent = -s_resolveTangent(rFrom, toPos);
                }
                const ImVec2 fromTangent = s_resolveTangent(rFrom, toPos);

                ImU32 color;
                if (rCtx.connAcceptedThisFrame || rCtx.connNewNodeAcceptedThisFrame) {
                    color = rCtx.connAcceptColor;
                } else if (rCtx.connRejectReason != nullptr) {
                    color = IM_COL32(255, 80, 80, 230);
                } else {
                    color = IM_COL32(220, 220, 220, 200);
                }

                if (rCtx.graphActive) {
                    auto* const pDrawList = rCtx.drawList;
                    s_setChannel(rCtx, GC_Overlay);
                    s_drawBezierLink(pDrawList, rFrom.screenPos, fromTangent, toPos, toTangent, color, 3.0f);
                    s_setChannel(rCtx, GC_Content);
                } else {
                    s_drawBezierLink(ImGui::GetWindowDrawList(), rFrom.screenPos, fromTangent, toPos, toTangent, color, 3.0f);
                }
            }
        }
    }

    // End the drag when the mouse is released.
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        rCtx.draggingFromSlot = 0;
    }
}

// =====================================================================
// Multi-selection
// =====================================================================

IMNODAL_API int GetSelectedObjectCount() {
    Context& rCtx = s_getCtx();
    if (!rCtx.graphActive) return 0;
    GraphState& g = rCtx.graphs[rCtx.currentGraphId];
    return (int)(g.selectedNodes.size() + g.selectedLinks.size());
}

IMNODAL_API int GetSelectedNodes(Id* apoBuffer, int aCapacity) {
    Context& rCtx = s_getCtx();
    if (!rCtx.graphActive || apoBuffer == nullptr || aCapacity <= 0) return 0;
    GraphState& g = rCtx.graphs[rCtx.currentGraphId];
    int n = 0;
    for (Id id : g.selectedNodes) {
        if (n >= aCapacity) break;
        apoBuffer[n++] = id;
    }
    return n;
}

IMNODAL_API int GetSelectedLinks(Id* apoBuffer, int aCapacity) {
    Context& rCtx = s_getCtx();
    if (!rCtx.graphActive || apoBuffer == nullptr || aCapacity <= 0) return 0;
    GraphState& g = rCtx.graphs[rCtx.currentGraphId];
    int n = 0;
    for (Id id : g.selectedLinks) {
        if (n >= aCapacity) break;
        apoBuffer[n++] = id;
    }
    return n;
}

IMNODAL_API bool HasSelectionChanged() {
    Context& rCtx = s_getCtx();
    if (!rCtx.graphActive) return false;
    return rCtx.graphs[rCtx.currentGraphId].selectionChangedThisFrame;
}

// AddToSelection / RemoveFromSelection look up the id type — node or link —
// so the host doesn't need to disambiguate.
IMNODAL_API void AddToSelection(Id aId) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.graphActive && "AddToSelection must be called inside BeginGraph/EndGraph");
    if (aId == 0) return;
    GraphState& g = rCtx.graphs[rCtx.currentGraphId];
    if (rCtx.nodes.count(aId) > 0) {
        if (g.selectedNodes.insert(aId).second) {
            g.lastSelectedNode = aId;
            g.selectionChangedThisFrame = true;
        }
    } else if (rCtx.links.count(aId) > 0) {
        if (g.selectedLinks.insert(aId).second) {
            g.lastSelectedLink = aId;
            g.selectionChangedThisFrame = true;
        }
    }
}

IMNODAL_API void RemoveFromSelection(Id aId) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.graphActive && "RemoveFromSelection must be called inside BeginGraph/EndGraph");
    if (aId == 0) return;
    GraphState& g = rCtx.graphs[rCtx.currentGraphId];
    if (g.selectedNodes.erase(aId) != 0) {
        if (g.lastSelectedNode == aId) {
            g.lastSelectedNode = g.selectedNodes.empty() ? 0 : *g.selectedNodes.begin();
        }
        g.selectionChangedThisFrame = true;
    }
    if (g.selectedLinks.erase(aId) != 0) {
        if (g.lastSelectedLink == aId) {
            g.lastSelectedLink = g.selectedLinks.empty() ? 0 : *g.selectedLinks.begin();
        }
        g.selectionChangedThisFrame = true;
    }
}

IMNODAL_API void ClearSelection() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.graphActive && "ClearSelection must be called inside BeginGraph/EndGraph");
    s_clearSelection(rCtx.graphs[rCtx.currentGraphId]);
}

// =====================================================================
// Direct hover queries
// =====================================================================
IMNODAL_API Id GetHoveredSlot() { return s_getCtx().currentHoveredSlot; }
IMNODAL_API Id GetHoveredNode() { return s_getCtx().currentHoveredNode; }
IMNODAL_API Id GetHoveredLink() { return s_getCtx().currentHoveredLink; }

// =====================================================================
// Context-menu requests
// =====================================================================
IMNODAL_API bool IsNodeContextMenuRequested(Id* apoNodeId) {
    Context& rCtx = s_getCtx();
    if (rCtx.ctxMenuNodeId == 0) return false;
    if (apoNodeId) *apoNodeId = rCtx.ctxMenuNodeId;
    return true;
}
IMNODAL_API bool IsSlotContextMenuRequested(Id* apoSlotId) {
    Context& rCtx = s_getCtx();
    if (rCtx.ctxMenuSlotId == 0) return false;
    if (apoSlotId) *apoSlotId = rCtx.ctxMenuSlotId;
    return true;
}
IMNODAL_API bool IsLinkContextMenuRequested(Id* apoLinkId) {
    Context& rCtx = s_getCtx();
    if (rCtx.ctxMenuLinkId == 0) return false;
    if (apoLinkId) *apoLinkId = rCtx.ctxMenuLinkId;
    return true;
}

// =====================================================================
// Delete state machine
// =====================================================================
IMNODAL_API bool BeginDelete() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.graphActive && "BeginDelete must be called inside BeginGraph/EndGraph");
    IM_ASSERT(!rCtx.deleteScopeOpen && "BeginDelete called twice without matching EndDelete");

    // Trigger: Delete or Backspace key, while the canvas is hovered.
    GraphState& g = rCtx.graphs[rCtx.currentGraphId];
    rCtx.pendingDeleteLinks.clear();
    rCtx.pendingDeleteNodes.clear();
    rCtx.acceptedDeleteLinks.clear();
    rCtx.acceptedDeleteNodes.clear();
    rCtx.currentDeleteCandidate = 0;
    rCtx.currentDeleteKind = 0;

    const bool ctrl          = ImGui::IsKeyDown(ImGuiMod_Ctrl) || ImGui::IsKeyDown(ImGuiMod_Super);
    const bool ctrlX         = ctrl && ImGui::IsKeyPressed(ImGuiKey_X, false);
    // repeat=false : on ne veut PAS que delete fire 10x si l'utilisateur tient
    // la touche enfoncee. Une seule deletion par appui.
    const bool deletePressed = ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
                               ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
                               ctrlX;
    const bool canvasHovered = rCtx.hovered;  // computed by EndCanvas of last frame
    // WantCaptureKeyboard : un widget ImGui (text input ailleurs, etc.) capte
    // le clavier. Backspace tape la-bas ne doit pas faire deleter notre
    // selection canvas — meme si la souris survole le canvas par hasard.
    const bool kbCaptured = ImGui::GetIO().WantCaptureKeyboard;
    if (deletePressed && canvasHovered && !kbCaptured) {
        for (Id id : g.selectedLinks) rCtx.pendingDeleteLinks.push_back(id);
        for (Id id : g.selectedNodes) rCtx.pendingDeleteNodes.push_back(id);
    }

    if (rCtx.pendingDeleteLinks.empty() && rCtx.pendingDeleteNodes.empty()) {
        return false;
    }
    rCtx.deleteScopeOpen = true;
    return true;
}

IMNODAL_API bool QueryDeletedLink(Id* apoLinkId) {
    Context& rCtx = s_getCtx();
    if (!rCtx.deleteScopeOpen) return false;
    if (rCtx.pendingDeleteLinks.empty()) {
        rCtx.currentDeleteCandidate = 0;
        rCtx.currentDeleteKind = 0;
        return false;
    }
    rCtx.currentDeleteCandidate = rCtx.pendingDeleteLinks.front();
    rCtx.currentDeleteKind = 1;
    if (apoLinkId) *apoLinkId = rCtx.currentDeleteCandidate;
    return true;
}

IMNODAL_API bool QueryDeletedNode(Id* apoNodeId) {
    Context& rCtx = s_getCtx();
    if (!rCtx.deleteScopeOpen) return false;
    if (rCtx.pendingDeleteNodes.empty()) {
        rCtx.currentDeleteCandidate = 0;
        rCtx.currentDeleteKind = 0;
        return false;
    }
    rCtx.currentDeleteCandidate = rCtx.pendingDeleteNodes.front();
    rCtx.currentDeleteKind = 2;
    if (apoNodeId) *apoNodeId = rCtx.currentDeleteCandidate;
    return true;
}

IMNODAL_API bool AcceptDelete() {
    Context& rCtx = s_getCtx();
    if (!rCtx.deleteScopeOpen || rCtx.currentDeleteCandidate == 0) return false;
    if (rCtx.currentDeleteKind == 1) {
        rCtx.acceptedDeleteLinks.push_back(rCtx.currentDeleteCandidate);
        rCtx.pendingDeleteLinks.pop_front();
    } else if (rCtx.currentDeleteKind == 2) {
        rCtx.acceptedDeleteNodes.push_back(rCtx.currentDeleteCandidate);
        rCtx.pendingDeleteNodes.pop_front();
    }
    rCtx.currentDeleteCandidate = 0;
    rCtx.currentDeleteKind = 0;
    return true;
}

IMNODAL_API void RejectDelete() {
    Context& rCtx = s_getCtx();
    if (!rCtx.deleteScopeOpen || rCtx.currentDeleteCandidate == 0) return;
    if (rCtx.currentDeleteKind == 1) {
        rCtx.pendingDeleteLinks.pop_front();
    } else if (rCtx.currentDeleteKind == 2) {
        rCtx.pendingDeleteNodes.pop_front();
    }
    rCtx.currentDeleteCandidate = 0;
    rCtx.currentDeleteKind = 0;
}

IMNODAL_API void EndDelete() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.deleteScopeOpen && "EndDelete without matching BeginDelete");
    // Drop accepted ids from the selection — the host will stop emitting them
    // next frame, but we want the selection state coherent immediately.
    if (rCtx.graphActive) {
        GraphState& g = rCtx.graphs[rCtx.currentGraphId];
        for (Id id : rCtx.acceptedDeleteLinks) g.selectedLinks.erase(id);
        for (Id id : rCtx.acceptedDeleteNodes) g.selectedNodes.erase(id);
        if (g.lastSelectedNode != 0 && g.selectedNodes.count(g.lastSelectedNode) == 0) {
            g.lastSelectedNode = g.selectedNodes.empty() ? 0 : *g.selectedNodes.begin();
        }
        if (g.lastSelectedLink != 0 && g.selectedLinks.count(g.lastSelectedLink) == 0) {
            g.lastSelectedLink = g.selectedLinks.empty() ? 0 : *g.selectedLinks.begin();
        }
        if (!rCtx.acceptedDeleteLinks.empty() || !rCtx.acceptedDeleteNodes.empty()) {
            g.selectionChangedThisFrame = true;
        }
    }
    rCtx.pendingDeleteLinks.clear();
    rCtx.pendingDeleteNodes.clear();
    rCtx.acceptedDeleteLinks.clear();
    rCtx.acceptedDeleteNodes.clear();
    rCtx.currentDeleteCandidate = 0;
    rCtx.currentDeleteKind = 0;
    rCtx.deleteScopeOpen = false;
}

// =====================================================================
// Shortcut machine
// =====================================================================

namespace {

// Snapshot the current selection into the action context arrays. Used as the
// "what does this shortcut act on?" payload.
static void s_captureActionContext(Context& arCtx) {
    arCtx.actionContextNodes.clear();
    arCtx.actionContextLinks.clear();
    if (!arCtx.graphActive) return;
    GraphState& g = arCtx.graphs[arCtx.currentGraphId];
    arCtx.actionContextNodes.reserve(g.selectedNodes.size());
    arCtx.actionContextLinks.reserve(g.selectedLinks.size());
    for (Id id : g.selectedNodes) arCtx.actionContextNodes.push_back(id);
    for (Id id : g.selectedLinks) arCtx.actionContextLinks.push_back(id);
}

}  // namespace

IMNODAL_API bool BeginShortcut() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.graphActive && "BeginShortcut must be called inside BeginGraph/EndGraph");
    IM_ASSERT(!rCtx.shortcutScopeOpen && "BeginShortcut called twice without matching EndShortcut");

    // Read shortcut keys only when the canvas is hovered — otherwise typing
    // Ctrl+C in a text input elsewhere would fire the graph's copy.
    if (!rCtx.hovered) return false;

    const bool ctrl = ImGui::IsKeyDown(ImGuiMod_Ctrl) || ImGui::IsKeyDown(ImGuiMod_Super);
    if (!ctrl) return false;

    // Detect each shortcut once per press.
    rCtx.shortcutCopyFired      = ImGui::IsKeyPressed(ImGuiKey_C, false);
    rCtx.shortcutPasteFired     = ImGui::IsKeyPressed(ImGuiKey_V, false);
    rCtx.shortcutCutFired       = ImGui::IsKeyPressed(ImGuiKey_X, false);
    rCtx.shortcutDuplicateFired = ImGui::IsKeyPressed(ImGuiKey_D, false);
    rCtx.shortcutSelectAllFired = ImGui::IsKeyPressed(ImGuiKey_A, false);

    const bool any = rCtx.shortcutCopyFired || rCtx.shortcutPasteFired ||
                     rCtx.shortcutCutFired  || rCtx.shortcutDuplicateFired ||
                     rCtx.shortcutSelectAllFired;
    if (!any) return false;

    // Select-all is a host-side operation but we can do it ourselves: insert
    // every node and every link of the current graph into the selection.
    if (rCtx.shortcutSelectAllFired) {
        GraphState& g = rCtx.graphs[rCtx.currentGraphId];
        const size_t before = g.selectedNodes.size() + g.selectedLinks.size();
        for (const auto& kv : rCtx.nodes) {
            if (kv.second.graphId == g.id) g.selectedNodes.insert(kv.first);
        }
        for (const auto& kv : rCtx.links) {
            if (kv.second.graphId == g.id) g.selectedLinks.insert(kv.first);
        }
        if (g.selectedNodes.size() + g.selectedLinks.size() != before) {
            g.selectionChangedThisFrame = true;
        }
    }

    s_captureActionContext(rCtx);
    rCtx.shortcutScopeOpen = true;
    return true;
}

IMNODAL_API bool AcceptCopy()      { return s_getCtx().shortcutCopyFired; }
IMNODAL_API bool AcceptPaste()     { return s_getCtx().shortcutPasteFired; }
IMNODAL_API bool AcceptCut()       { return s_getCtx().shortcutCutFired; }
IMNODAL_API bool AcceptDuplicate() { return s_getCtx().shortcutDuplicateFired; }
IMNODAL_API bool AcceptSelectAll() { return s_getCtx().shortcutSelectAllFired; }

IMNODAL_API void EndShortcut() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.shortcutScopeOpen && "EndShortcut without matching BeginShortcut");
    rCtx.shortcutScopeOpen = false;
    // Action-context arrays are kept around for queries this frame; they
    // are reset by the next NewFrame (or the next BeginShortcut).
}

IMNODAL_API int GetActionContextNodes(Id* apoBuffer, int aCapacity) {
    Context& rCtx = s_getCtx();
    if (apoBuffer == nullptr || aCapacity <= 0) return 0;
    const int n = ImMin((int)rCtx.actionContextNodes.size(), aCapacity);
    for (int i = 0; i < n; ++i) apoBuffer[i] = rCtx.actionContextNodes[i];
    return n;
}

IMNODAL_API int GetActionContextLinks(Id* apoBuffer, int aCapacity) {
    Context& rCtx = s_getCtx();
    if (apoBuffer == nullptr || aCapacity <= 0) return 0;
    const int n = ImMin((int)rCtx.actionContextLinks.size(), aCapacity);
    for (int i = 0; i < n; ++i) apoBuffer[i] = rCtx.actionContextLinks[i];
    return n;
}

// =====================================================================
// Flow animation on a link
// =====================================================================
//
// The flow is rendered as N small dots travelling along the link's bezier.
// We reuse the cached control points captured by Link() this frame — that's
// why FlowLink() must be called AFTER the matching Link() call.

IMNODAL_API void FlowLink(Id aLinkId, float aSpeed, ImU32 aColor) {
    Context& rCtx = s_getCtx();
    auto it = rCtx.links.find(aLinkId);
    if (it == rCtx.links.end()) return;
    LinkState& rLink = it->second;
    if (!rLink.bezierCached) return;  // Link() wasn't called this frame

    constexpr int kDotCount = 5;
    constexpr int kSamples  = 32;

    // Phase progresses every frame, wraps over [0, 1).
    const float t = static_cast<float>(ImGui::GetTime());
    const float canvasScale = rCtx.scale > 0.0f ? rCtx.scale : 1.0f;
    // Approximate curve length via the chord (good enough for animation phase).
    const ImVec2 chord = rLink.cachedToPos - rLink.cachedFromPos;
    const float chordLen = std::sqrt(chord.x * chord.x + chord.y * chord.y) + 1e-3f;
    const float phase = std::fmod(t * aSpeed * canvasScale / chordLen, 1.0f);

    const ImU32 color = (aColor != 0) ? aColor
                       : IM_COL32(255, 220, 120, 255);
    const float dotRadius = ImMax(rLink.thickness * 0.9f, 2.0f);

    auto* const pDrawList = rCtx.graphActive ? rCtx.drawList : ImGui::GetWindowDrawList();
    if (rCtx.graphActive) s_setChannel(rCtx, GC_Links);

    for (int i = 0; i < kDotCount; ++i) {
        float u = phase + (float)i / (float)kDotCount;
        if (u >= 1.0f) u -= 1.0f;
        const float v = 1.0f - u;
        const ImVec2 pt =
            rLink.cachedFromPos * (v * v * v) +
            rLink.cachedP1     * (3.0f * v * v * u) +
            rLink.cachedP2     * (3.0f * v * u * u) +
            rLink.cachedToPos  * (u * u * u);
        pDrawList->AddCircleFilled(pt, dotRadius, color);
        (void)kSamples;
    }

    if (rCtx.graphActive) s_setChannel(rCtx, GC_Content);
}

// =====================================================================
// Per-node draw lists
// =====================================================================
// These return the canvas's draw list with the active channel switched to
// the matching z-level. The channel is restored to GC_Content at the next
// EndNode / EndSlot / Link / etc. that runs through s_setChannel.
//
// IMPORTANT: do NOT emit ImGui widgets after calling these until the next
// ImNodal call — the widget would land on the wrong channel.

IMNODAL_API ImDrawList* GetNodeBackgroundDrawList(Id aNodeId) {
    Context& rCtx = s_getCtx();
    if (!rCtx.graphActive) return ImGui::GetWindowDrawList();
    if (rCtx.nodes.count(aNodeId) == 0) return rCtx.drawList;
    s_setChannel(rCtx, GC_UserBg);
    return rCtx.drawList;
}

IMNODAL_API ImDrawList* GetNodeForegroundDrawList(Id aNodeId) {
    Context& rCtx = s_getCtx();
    if (!rCtx.graphActive) return ImGui::GetWindowDrawList();
    if (rCtx.nodes.count(aNodeId) == 0) return rCtx.drawList;
    s_setChannel(rCtx, GC_UserFg);
    return rCtx.drawList;
}

IMNODAL_API ImRect GetNodeRect(Id aNodeId) {
    Context& rCtx = s_getCtx();
    auto it = rCtx.nodes.find(aNodeId);
    if (it == rCtx.nodes.end()) return ImRect();
    const ImRect& r = it->second.lastScreenRect;
    // The cached rect is in LOCAL space (captured during EndNode while inside
    // BeginCanvas local-space scope). When the caller is outside that scope
    // — i.e. canvas inactive or suspended — translate to screen so the rect
    // is directly usable for ImGui screen-space draws / popups.
    const bool inLocal = rCtx.active && rCtx.suspendCounter == 0;
    if (inLocal) return r;
    return ImRect(s_canvasToScreen(rCtx, r.Min), s_canvasToScreen(rCtx, r.Max));
}

// =====================================================================
// Slot dot pivot
// =====================================================================
IMNODAL_API void SlotDotPivotAlignment(const ImVec2& aAlignment) {
    s_getCtx().slotPivotAlignment = aAlignment;
}
IMNODAL_API void SlotDotPivotOffset(const ImVec2& aOffsetPx) {
    s_getCtx().slotPivotOffset = aOffsetPx;
}
IMNODAL_API void SlotDotAnchorEdge(SlotDotEdge aEdge) {
    Context& rCtx = s_getCtx();
    switch (aEdge) {
    case SlotDotEdge_Left:   rCtx.slotPivotAlignment = ImVec2(0.0f, 0.5f); break;
    case SlotDotEdge_Right:  rCtx.slotPivotAlignment = ImVec2(1.0f, 0.5f); break;
    case SlotDotEdge_Top:    rCtx.slotPivotAlignment = ImVec2(0.5f, 0.0f); break;
    case SlotDotEdge_Bottom: rCtx.slotPivotAlignment = ImVec2(0.5f, 1.0f); break;
    case SlotDotEdge_Center: rCtx.slotPivotAlignment = ImVec2(0.5f, 0.5f); break;
    case SlotDotEdge_Auto:
    default:                 rCtx.slotPivotAlignment = ImVec2(-1.0f, -1.0f); break;
    }
}

// =====================================================================
// Navigation
// =====================================================================

namespace {

// Build the canvas-space bbox of a set of node ids. Returns false if no node
// in the set has a known size yet (e.g. before the first frame ran).
static bool s_collectNodeBBox(const Context& arCtx, const std::unordered_set<Id>& arSet,
                              ImVec2& aroMin, ImVec2& aroMax) {
    bool any = false;
    for (Id id : arSet) {
        auto it = arCtx.nodes.find(id);
        if (it == arCtx.nodes.end()) continue;
        const NodeState& n = it->second;
        if (n.size.x <= 0.0f || n.size.y <= 0.0f) continue;
        const ImVec2 lo = n.pos;
        const ImVec2 hi = n.pos + n.size;
        if (!any) {
            aroMin = lo;
            aroMax = hi;
            any = true;
        } else {
            aroMin.x = ImMin(aroMin.x, lo.x);
            aroMin.y = ImMin(aroMin.y, lo.y);
            aroMax.x = ImMax(aroMax.x, hi.x);
            aroMax.y = ImMax(aroMax.y, hi.y);
        }
    }
    return any;
}

static bool s_collectAllNodesBBox(const Context& arCtx, ImVec2& aroMin, ImVec2& aroMax) {
    bool any = false;
    for (const auto& kv : arCtx.nodes) {
        const NodeState& n = kv.second;
        if (n.size.x <= 0.0f || n.size.y <= 0.0f) continue;
        const ImVec2 lo = n.pos;
        const ImVec2 hi = n.pos + n.size;
        if (!any) {
            aroMin = lo;
            aroMax = hi;
            any = true;
        } else {
            aroMin.x = ImMin(aroMin.x, lo.x);
            aroMin.y = ImMin(aroMin.y, lo.y);
            aroMax.x = ImMax(aroMax.x, hi.x);
            aroMax.y = ImMax(aroMax.y, hi.y);
        }
    }
    return any;
}

}  // namespace

IMNODAL_API void NavigateToContent(bool aZoomToFit, float aMarginRatio) {
    Context& rCtx = s_getCtx();
    ImVec2 lo, hi;
    if (!s_collectAllNodesBBox(rCtx, lo, hi)) return;
    if (aZoomToFit) {
        ZoomCanvasToRect(lo, hi, aMarginRatio);
    } else {
        const ImVec2 center((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        CenterCanvasOn(center);
    }
}

IMNODAL_API void NavigateToSelection(bool aZoomToFit, float aMarginRatio) {
    Context& rCtx = s_getCtx();
    if (!rCtx.graphActive) return;
    GraphState& g = rCtx.graphs[rCtx.currentGraphId];
    if (g.selectedNodes.empty()) {
        // Empty selection → fall back to "navigate to content".
        NavigateToContent(aZoomToFit, aMarginRatio);
        return;
    }
    ImVec2 lo, hi;
    if (!s_collectNodeBBox(rCtx, g.selectedNodes, lo, hi)) return;
    if (aZoomToFit) {
        ZoomCanvasToRect(lo, hi, aMarginRatio);
    } else {
        const ImVec2 center((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        CenterCanvasOn(center);
    }
}

// =====================================================================
// Reroute node (M2)
// =====================================================================
// Minimal 1-in/1-out pass-through node rendered as a single dot. Draggable;
// clicking on either slot starts a new connection drag.

IMNODAL_API bool BeginRerouteNode(Id aNodeId, Id aSlotId, ImVec2* apPos, const NodeSettings& arSettings, ImU32 aDotColor) {
    NodeSettings reSettings = arSettings;
    // Reroute UE-style : un dot bien visible + une frame circulaire LEGEREMENT
    // transparente en permanence pour materialiser la zone draggable, meme
    // au repos (sans polluer le rendu — l'alpha est faible). Quand le reroute
    // est selectionne, la frame passe en jaune semi-opaque (selectedBorderColor).
    // Le body et le header restent totalement transparents.
    reSettings.bodyColor           = IM_COL32(0, 0, 0, 0);
    reSettings.headerColor         = IM_COL32(0, 0, 0, 0);
    reSettings.borderColor         = IM_COL32(220, 220, 220, 70);   // faint white-ish frame
    reSettings.selectedBorderColor = IM_COL32(255, 180, 0, 200);    // yellow, slightly transparent
    reSettings.borderThickness     = 1.5f;
    reSettings.rounding            = 3.0f;
    reSettings.drawHoverHandle     = false;

    if (!BeginNode(aNodeId, apPos, reSettings)) {
        return false;
    }

    // Marque le node comme reroute pour que EndNode dessine la frame de
    // selection en CERCLE (au lieu du rect par defaut), et pour que le halo
    // de hover du slot interne soit aussi un cercle dans EndGraph.
    {
        Context& rCtx = s_getCtx();
        auto it = rCtx.nodes.find(aNodeId);
        if (it != rCtx.nodes.end()) {
            it->second.isReroute = true;
            it->second.rerouteSlotId = aSlotId;
        }
    }

    // Slot dot color : if the host passed a link-matching color, use it for
    // the connected/default state so the dot adopts the wire color (UE-like).
    SlotSettings dotSettings{};
    dotSettings.dotRadius = 5.0f;
    if (aDotColor != 0) {
        dotSettings.dotColor          = aDotColor;
        dotSettings.dotColorConnected = aDotColor;
        // Hovered : a slight white tint blended with the link color reads as
        // "highlighted" without losing the link's identity.
        dotSettings.dotColorHovered   = IM_COL32(255, 255, 255, 255);
    }

    // Leading top spacer so the hover handle has visual room above the dot.
    ImGui::Dummy(ImVec2(18.0f, 4.0f));
    BeginSlot(aSlotId, SlotRole_InOut, "", dotSettings);
    ImGui::Dummy(ImVec2(14.0f, 8.0f));
    EndSlot();
    // Trailing bottom spacer so the drag zone extends below the dot too.
    ImGui::Dummy(ImVec2(18.0f, 2.0f));

    return true;
}

IMNODAL_API void EndRerouteNode() {
    EndNode();
}

}  // namespace ImNodal
