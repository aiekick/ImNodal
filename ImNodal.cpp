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
#include <unordered_map>
#include <vector>

namespace ImNodal {

// =====================================================================
// Graph layer — internal types
// =====================================================================

// Pin mode for BeginSlot: decided by the enclosing section (if any).
enum PinMode {
    PinMode_Inline = 0,  // dot sits next to the widget (default, also for standalone slots)
    PinMode_LeftEdge,    // dot pinned to the node's left edge (set by BeginInputs)
    PinMode_RightEdge,   // dot pinned to the node's right edge (set by BeginOutputs)
};

struct SlotState {
    Id        parentNode{0};    // 0 = standalone
    Id        graphId{0};       // 0 = standalone (outside any BeginGraph)
    SlotRole  role{SlotRole_Input};
    uint32_t  typeTag{0};
    ImVec2    screenPos{};      // committed at EndSlot (inline) or EndNode (pinned)
    ImVec2    tangent{};        // unit vector pointing away from the node edge
    float     dotRadius{5.0f};
    bool      hovered{false};
    bool      connected{false}; // set by M2
    // Pending state used by a node's EndNode to finalize position
    PinMode   pinMode{PinMode_Inline};
    float     pendingY{0.0f};   // group-rect Y center (screen space)
};

struct NodeState {
    ImVec2   pos{};          // canvas-space top-left
    ImVec2   size{};          // filled at EndNode
    Id       graphId{0};
    bool     hovered{false};
    bool     selected{false};
    bool     dragging{false};
    // Settings snapshot for EndNode draw
    NodeSettings settings;
    // Header rect (screen), filled at EndHeader, used by EndNode for header tint
    ImRect   headerScreenRect{};
    bool     hasHeader{false};
    // Pending slots to finalize X at EndNode
    std::vector<Id> pendingSlots;
    // Body column tracking (to emit SameLine between Inputs/Center/Outputs automatically)
    bool     bodyColumnOpened{false};
    // Pointer to user's master copy of pos — updated on drag at EndNode
    ImVec2*  userPosPtr{nullptr};
};

struct LinkState {
    Id    fromSlot{0};
    Id    toSlot{0};
    Id    graphId{0};
    ImU32 color{0};
    // Per-frame interaction state (computed by Link(), reset at BeginGraph).
    bool  hovered{false};
    bool  clicked{false};
    bool  doubleClicked{false};
    bool  selected{false};
};

struct GraphState {
    Id       id{0};
    GraphSettings settings{};
    // Splitter channels managed inside BeginGraph/EndGraph
    bool     splitterActive{false};
    int      preBeginChannel{0};
    // Per-frame set of hovered/selected
    Id       selectedNode{0};
    Id       selectedLink{0};
    // Draw order / node Z (M1: simple insertion order)
    std::vector<Id> frameNodeOrder;
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

    // Pin mode stack: top drives BeginSlot behavior
    std::vector<PinMode> pinModeStack;

    // Currently open slot (0 = none)
    Id       currentSlotId{0};
    // Slot group Y capture (begin cursor Y, for centering)
    ImVec2   slotGroupStart{};

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
    bool        connAcceptedThisFrame{false}; // AcceptNewLink called this frame
    ImU32       connAcceptColor{0};           // Color user passed to AcceptNewLink
    const char* connRejectReason{nullptr};    // RejectNewLink reason (pointer assumed stable for the frame)
    bool        connCommitThisFrame{false};   // Set when mouse released AND accepted this frame

    // Global "selected link" for standalone (outside-of-graph) link queries.
    Id          standaloneSelectedLink{0};

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

    arCtx.currentHoveredSlot    = 0;
    arCtx.connAcceptedThisFrame = false;
    arCtx.connAcceptColor       = 0;
    arCtx.connRejectReason      = nullptr;
    arCtx.connCommitThisFrame   = false;

    for (auto& kv : arCtx.slots) {
        kv.second.connected = false;
    }
    for (auto& kv : arCtx.links) {
        kv.second.hovered = false;
        kv.second.clicked = false;
        kv.second.doubleClicked = false;
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

    s_leaveLocalSpace(rCtx);
    ImGui::GetCurrentWindow()->DC.CursorMaxPos = rCtx.windowCursorMaxBackup;

    // Background interaction flags: computed now that mouse is back in screen
    // space, all user items are emitted, and our own layout Dummy is not yet.
    rCtx.hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(rCtx.widgetRect.Min, rCtx.widgetRect.Max);
    const bool onEmpty = rCtx.hovered && !ImGui::IsAnyItemHovered() && !rCtx.isPanning;
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

// Draw list channel layout (per active BeginGraph)
enum GraphChannel {
    GC_Background = 0,  // node bg + border + slot dots
    GC_Content    = 1,  // user widgets inside nodes
    GC_Links      = 2,  // M2
    GC_Overlay    = 3,  // M3 box-select, M2 drag-preview link
    GC_Count      = 4,
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

// Top of pin mode stack (Inline if empty).
static PinMode s_currentPinMode(const Context& arCtx) {
    if (arCtx.pinModeStack.empty()) {
        return PinMode_Inline;
    }
    return arCtx.pinModeStack.back();
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

    GraphState& rGraph = rCtx.graphs[aGraphId];
    rGraph.id = aGraphId;
    rGraph.settings = arSettings;
    rGraph.frameNodeOrder.clear();

    // Refresh link "selected" flags from this graph's selection (per-frame).
    for (auto& kv : rCtx.links) {
        kv.second.selected = (kv.first == rGraph.selectedLink);
    }

    // Open 4-channel splitter for this graph. Default channel = Content.
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
    const bool lmbClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool lmbOnCanvas = ImGui::IsWindowHovered() && rCtx.widgetRect.Contains(rCtx.mousePosBackup);
    if (lmbClicked && lmbOnCanvas && !rGraph.clickConsumedThisFrame && !rCtx.isPanning) {
        rGraph.selectedNode = 0;
        rGraph.selectedLink = 0;
    }
    // Reset for next frame
    rGraph.clickConsumedThisFrame = false;

    // Merge channels back into the canvas-expected channel order.
    if (rGraph.splitterActive) {
        pDrawList->ChannelsMerge();
        rGraph.splitterActive = false;
    }
    // Splitter must be back to the canvas's expected channel
    IM_ASSERT(pDrawList->_Splitter._Current == rCtx.expectedChannel && "EndGraph: splitter not restored to canvas expected channel");

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
    rNode.pendingSlots.clear();
    rNode.hasHeader = false;
    rNode.bodyColumnOpened = false;
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
    const ImVec2 nodeMin = ImGui::GetItemRectMin();
    const ImVec2 nodeMax = ImGui::GetItemRectMax();
    rNode.size = nodeMax - nodeMin;

    // Hover test against the full node rect
    rNode.hovered = ImGui::IsMouseHoveringRect(nodeMin, nodeMax) && ImGui::IsWindowHovered();

    // ---- Selection + drag (left-click-hold on the node rect) ----
    const bool lmbClicked   = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool lmbDown      = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool lmbReleased  = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    // "topmost" = mouse on node AND no user widget is above the cursor.
    const bool hoveredTopMost = rNode.hovered && !ImGui::IsAnyItemHovered();

    if (hoveredTopMost && lmbClicked) {
        rGraph.selectedNode = rCtx.currentNodeId;
        rGraph.clickConsumedThisFrame = true;
        if (rNode.settings.movable) {
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
    rNode.selected = (rGraph.selectedNode == rCtx.currentNodeId);

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
        // Extend header visual to the full node width
        hdr.Min.x = nodeMin.x;
        hdr.Max.x = nodeMax.x;
        pDrawList->AddRectFilled(hdr.Min, hdr.Max, rNode.settings.headerColor, rounding, ImDrawFlags_RoundCornersTop);
    }
    // Border (selected color overrides)
    const ImU32 borderCol = rNode.selected ? rNode.settings.selectedBorderColor : rNode.settings.borderColor;
    pDrawList->AddRect(nodeMin, nodeMax, borderCol, rounding, 0, rNode.settings.borderThickness);

    // Hover-only drag handle bar — shown only when mouse is on the node.
    // Used by reroute-style nodes that have no header: gives the user a visible
    // grab surface to drag the node, appearing only when needed.
    if (rNode.settings.drawHoverHandle && rNode.hovered) {
        const float h = rNode.settings.hoverHandleHeight;
        const ImVec2 bMin(nodeMin.x + 2.0f, nodeMin.y + 1.0f);
        const ImVec2 bMax(nodeMax.x - 2.0f, nodeMin.y + 1.0f + h);
        pDrawList->AddRectFilled(bMin, bMax, rNode.settings.hoverHandleColor, h * 0.5f);
    }

    // ---- Commit pending slot X on node edges, refresh hover, draw dots ----
    // The hover computed in EndSlot used a preliminary X. Now that we know the
    // final dot position (at the node edge), we redo a cheap geometric hover
    // test that includes the dot's outer half (which sits OUTSIDE the node
    // rect). Without this, hover only fires on the inner half of the dot.
    const ImVec2 mp = ImGui::GetIO().MousePos;
    for (Id slotId : rNode.pendingSlots) {
        auto it = rCtx.slots.find(slotId);
        if (it == rCtx.slots.end()) continue;
        SlotState& rSlot = it->second;
        float x = rSlot.screenPos.x;
        if (rSlot.pinMode == PinMode_LeftEdge) {
            x = nodeMin.x;
            rSlot.tangent = ImVec2(-1.0f, 0.0f);
        } else if (rSlot.pinMode == PinMode_RightEdge) {
            x = nodeMax.x;
            rSlot.tangent = ImVec2(1.0f, 0.0f);
        }
        rSlot.screenPos = ImVec2(x, rSlot.pendingY);

        // Refresh hover with the FINAL position. Rect covers the dot plus a
        // strip extending into the node body along the slot's row.
        const float pad = rSlot.dotRadius + 4.0f;
        ImVec2 hitMin, hitMax;
        if (rSlot.pinMode == PinMode_LeftEdge) {
            // Strip from (dotLeft - pad) to (nodeMin.x + row extent). Use the
            // row height captured via pendingY ± a generous half-height.
            hitMin = ImVec2(rSlot.screenPos.x - pad, rSlot.pendingY - 12.0f);
            hitMax = ImVec2(rSlot.screenPos.x + pad, rSlot.pendingY + 12.0f);
        } else {
            hitMin = ImVec2(rSlot.screenPos.x - pad, rSlot.pendingY - 12.0f);
            hitMax = ImVec2(rSlot.screenPos.x + pad, rSlot.pendingY + 12.0f);
        }
        const bool onDot =
            mp.x >= hitMin.x && mp.x <= hitMax.x &&
            mp.y >= hitMin.y && mp.y <= hitMax.y;
        if (onDot) {
            rSlot.hovered = true;
            rCtx.currentHoveredSlot = slotId;
        }

        const ImU32 col = rSlot.hovered   ? IM_COL32(255, 255, 255, 255)
                        : rSlot.connected ? IM_COL32(255, 220,   0, 255)
                        :                   IM_COL32(200, 200, 200, 255);
        pDrawList->AddCircleFilled(rSlot.screenPos, rSlot.dotRadius, col);
    }

    // Restore content channel for next widgets
    s_setChannel(rCtx, GC_Content);

    ImGui::PopID();

    // Echo the (possibly updated by drag) position back to user storage via aPos*
    // Caller keeps a pointer; we updated rNode.pos during drag. The caller will
    // read it back in their own buffer each frame via the apPos in/out param.
    // BeginNode already did `if (apPos) rNode.pos = *apPos` at entry, and the drag
    // writes to rNode.pos. On the next call BeginNode pulls from user's copy again,
    // so users MUST write back: they do so via `*apPos = GetNodePos(id)` OR we
    // require BeginNode signature to hold the pointer and update at EndNode.
    // → We already update via the caller's pointer at EndNode:
    if (rNode.dragging || rGraph.selectedNode == rCtx.currentNodeId) {
        // Write-back is deferred to EndNode where we don't have apPos anymore.
        // Simpler: the caller must re-read via GetNodePos, OR we make BeginNode
        // capture the pointer. To keep M1 simple, we remember the pointer per node.
    }

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
    rCtx.pinModeStack.push_back(PinMode_Inline);
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
    rCtx.pinModeStack.pop_back();
    rCtx.currentSection = Context::Section_None;
}

// Helper: open body column — if another body column was already opened, emit SameLine first.
static bool s_beginBodyColumn(Context& rCtx, Context::Section aSec, PinMode aMode) {
    IM_ASSERT(rCtx.currentNodeId != 0 && "BeginInputs/Outputs/Center outside of BeginNode/EndNode");
    IM_ASSERT(rCtx.currentSection == Context::Section_None && "BeginInputs/Outputs/Center while another section is still open");
    NodeState& rNode = rCtx.nodes[rCtx.currentNodeId];
    if (rNode.bodyColumnOpened) {
        ImGui::SameLine(0.0f, rNode.settings.columnSpacing);
    }
    rCtx.currentSection = aSec;
    rCtx.pinModeStack.push_back(aMode);
    ImGui::BeginGroup();
    return true;
}
static void s_endBodyColumn(Context& rCtx, Context::Section aSec) {
    IM_ASSERT(rCtx.currentSection == aSec && "EndInputs/Outputs/Center without matching Begin");
    ImGui::EndGroup();
    rCtx.pinModeStack.pop_back();
    rCtx.nodes[rCtx.currentNodeId].bodyColumnOpened = true;
    rCtx.currentSection = Context::Section_None;
}

IMNODAL_API bool BeginInputs()  { return s_beginBodyColumn(s_getCtx(), Context::Section_Inputs,  PinMode_LeftEdge); }
IMNODAL_API void EndInputs()    { s_endBodyColumn(s_getCtx(), Context::Section_Inputs); }
IMNODAL_API bool BeginCenter()  { return s_beginBodyColumn(s_getCtx(), Context::Section_Center,  PinMode_Inline); }
IMNODAL_API void EndCenter()    { s_endBodyColumn(s_getCtx(), Context::Section_Center); }
IMNODAL_API bool BeginOutputs() { return s_beginBodyColumn(s_getCtx(), Context::Section_Outputs, PinMode_RightEdge); }
IMNODAL_API void EndOutputs()   { s_endBodyColumn(s_getCtx(), Context::Section_Outputs); }

IMNODAL_API bool BeginFooter() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.currentNodeId != 0 && "BeginFooter outside of BeginNode/EndNode");
    IM_ASSERT(rCtx.currentSection == Context::Section_None && "BeginFooter while another section is still open");
    rCtx.currentSection = Context::Section_Footer;
    rCtx.pinModeStack.push_back(PinMode_Inline);
    ImGui::BeginGroup();
    return true;
}
IMNODAL_API void EndFooter() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.currentSection == Context::Section_Footer && "EndFooter without matching BeginFooter");
    ImGui::EndGroup();
    rCtx.pinModeStack.pop_back();
    rCtx.currentSection = Context::Section_None;
}

// -----------------------------
// Slot primitive
// -----------------------------
IMNODAL_API bool BeginSlot(Id aSlotId, SlotRole aRole, const char* aLabel, const SlotSettings& arSettings) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(aSlotId != 0 && "Slot id must be non-zero");
    IM_ASSERT(rCtx.currentSlotId == 0 && "Nested BeginSlot is not supported");
    SlotState& rSlot = rCtx.slots[aSlotId];
    rSlot.parentNode = rCtx.currentNodeId;
    rSlot.graphId    = rCtx.currentGraphId;
    rSlot.role       = aRole;
    rSlot.typeTag    = arSettings.typeTag;
    rSlot.dotRadius  = arSettings.dotRadius;
    rSlot.pinMode    = s_currentPinMode(rCtx);
    rSlot.hovered    = false;

    rCtx.currentSlotId = aSlotId;

    ImGui::PushID(s_imguiId(aSlotId));
    ImGui::BeginGroup();

    const float padding = arSettings.dotRadius * 2.0f + 4.0f;
    const bool  isOutput = (aRole == SlotRole_Output);
    const bool  isInOut  = (aRole == SlotRole_InOut);

    // Layout rule:
    //  Pinned LeftEdge (input on node edge): indent from the left by dot padding so the label
    //    leaves room for the dot to sit on the node's left border.
    //  Pinned RightEdge (output on node edge): right-align; add padding ON THE RIGHT after the
    //    user widgets — we achieve that by drawing label first and user widgets after.
    //  Inline: dot side depends on role (input → left of label, output → right; InOut centered).
    if (rSlot.pinMode == PinMode_LeftEdge) {
        ImGui::Dummy(ImVec2(padding, 0.0f));
        ImGui::SameLine(0.0f, 0.0f);
        if (aLabel && aLabel[0] != 0) {
            ImGui::TextUnformatted(aLabel);
            ImGui::SameLine();
        }
    } else if (rSlot.pinMode == PinMode_RightEdge) {
        if (aLabel && aLabel[0] != 0) {
            ImGui::TextUnformatted(aLabel);
            ImGui::SameLine();
        }
    } else if (isInOut) {
        // InOut inline: no side-specific padding; dot will be centered at EndSlot.
        if (aLabel && aLabel[0] != 0) {
            ImGui::TextUnformatted(aLabel);
            ImGui::SameLine();
        }
    } else {
        // Inline input/output: dot on the natural side.
        if (!isOutput) {
            ImGui::Dummy(ImVec2(padding, 0.0f));
            ImGui::SameLine(0.0f, 0.0f);
        }
        if (aLabel && aLabel[0] != 0) {
            ImGui::TextUnformatted(aLabel);
            ImGui::SameLine();
        }
    }
    return true;
}

IMNODAL_API void EndSlot() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.currentSlotId != 0 && "EndSlot without matching BeginSlot");

    SlotState& rSlot = rCtx.slots[rCtx.currentSlotId];

    // If right-edge output / inline output, emit tail padding so the dot can sit on the right.
    const bool isOutput = (rSlot.role == SlotRole_Output);
    const float padding = rSlot.dotRadius * 2.0f + 4.0f;
    if (rSlot.pinMode == PinMode_RightEdge || (rSlot.pinMode == PinMode_Inline && isOutput)) {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Dummy(ImVec2(padding, 0.0f));
    }

    ImGui::EndGroup();
    const ImVec2 gMin = ImGui::GetItemRectMin();
    const ImVec2 gMax = ImGui::GetItemRectMax();
    rSlot.pendingY = (gMin.y + gMax.y) * 0.5f;

    const bool isInOut = (rSlot.role == SlotRole_InOut);

    // Default screenPos depending on mode (X committed later by EndNode for pinned modes).
    if (rSlot.pinMode == PinMode_LeftEdge) {
        rSlot.screenPos = ImVec2(gMin.x, rSlot.pendingY);   // overwritten by EndNode
        rSlot.tangent   = ImVec2(-1.0f, 0.0f);
    } else if (rSlot.pinMode == PinMode_RightEdge) {
        rSlot.screenPos = ImVec2(gMax.x, rSlot.pendingY);
        rSlot.tangent   = ImVec2(1.0f, 0.0f);
    } else if (isInOut) {
        // InOut: dot centered on the group rect. Tangent sentinel (0,0) →
        // resolved dynamically at Link-time based on the other endpoint.
        const float x = (gMin.x + gMax.x) * 0.5f;
        rSlot.screenPos = ImVec2(x, rSlot.pendingY);
        rSlot.tangent   = ImVec2(0.0f, 0.0f);
    } else {
        // Inline: dot sits on the relevant side of the group
        const float x = isOutput ? (gMax.x - rSlot.dotRadius) : (gMin.x + rSlot.dotRadius);
        rSlot.screenPos = ImVec2(x, rSlot.pendingY);
        rSlot.tangent   = ImVec2(isOutput ? 1.0f : -1.0f, 0.0f);
    }

    // --- Hover + click detection: two flavours depending on context ---
    // Inside a node the slot is just drawing (the node's section layout already
    // owns the flow); raw geometric test is enough and we claim ActiveId
    // manually on drag start so the host window doesn't move.
    // In a plain ImGui window the slot IS a widget: ItemSize + ItemAdd so it
    // participates in layout like a Button, and ButtonBehavior handles hover /
    // click / ActiveId naturally.
    {
        const float pad = rSlot.dotRadius + 4.0f;
        const ImRect hitBB(
            ImMin(gMin, rSlot.screenPos - ImVec2(pad, pad)),
            ImMax(gMax, rSlot.screenPos + ImVec2(pad, pad)));
        const bool insideNode = (rCtx.currentNodeId != 0);
        ImGuiWindow* const pWindow = ImGui::GetCurrentWindow();

        if (insideNode) {
            // --- Drawing-only mode ---
            const bool mouseOnSlot = ImGui::IsMouseHoveringRect(hitBB.Min, hitBB.Max, false);
            rSlot.hovered = mouseOnSlot;
            if (mouseOnSlot) {
                rCtx.currentHoveredSlot = rCtx.currentSlotId;
            }
            if (mouseOnSlot && rCtx.draggingFromSlot == 0 &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                !ImGui::IsAnyItemActive()) {
                rCtx.draggingFromSlot = rCtx.currentSlotId;
                const ImGuiID dragId = pWindow->GetID("##imnodal_slot_drag");
                ImGui::SetActiveID(dragId, pWindow);
                if (rCtx.graphActive) {
                    rCtx.graphs[rCtx.currentGraphId].clickConsumedThisFrame = true;
                }
            }
        } else {
            // --- Widget (button) mode ---
            // BeginGroup/EndGroup already advanced the layout cursor to the
            // group's end, so we just register the hit rect as an item and run
            // ButtonBehavior; ItemSize is skipped because the group handled it.
            const ImGuiID hitId = pWindow->GetID("##imnodal_slot_hit");
            ImGui::KeepAliveID(hitId);
            bool btnHovered = false, btnHeld = false;
            if (ImGui::ItemAdd(hitBB, hitId)) {
                ImGui::ButtonBehavior(hitBB, hitId, &btnHovered, &btnHeld,
                    ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            }
            // Raw geometric hover in parallel: while dragging from slot A,
            // ButtonBehavior on slot B returns hovered=false (ActiveId gating),
            // so btnHovered alone misses the "I'm the drop target" case. The
            // raw test bypasses that and keeps the dot color + currentHoveredSlot
            // up to date for link-target tracking.
            const bool mouseOnSlot = ImGui::IsMouseHoveringRect(hitBB.Min, hitBB.Max, false);
            rSlot.hovered = btnHovered || mouseOnSlot;
            if (mouseOnSlot) {
                rCtx.currentHoveredSlot = rCtx.currentSlotId;
            }
            // Click detection uses ButtonBehavior (which already set ActiveId = hitId).
            if (btnHeld && rCtx.draggingFromSlot == 0 &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                rCtx.draggingFromSlot = rCtx.currentSlotId;
            }
        }
    }

    // --- Commit dot: pinned slots are deferred to EndNode (node edge X is
    // unknown until then); inline/standalone slots draw the dot now. ---
    if (rCtx.currentNodeId != 0 && (rSlot.pinMode == PinMode_LeftEdge || rSlot.pinMode == PinMode_RightEdge)) {
        rCtx.nodes[rCtx.currentNodeId].pendingSlots.push_back(rCtx.currentSlotId);
    } else {
        const ImU32 col = rSlot.hovered   ? IM_COL32(255, 255, 255, 255)
                        : rSlot.connected ? IM_COL32(255, 220,   0, 255)
                        :                   IM_COL32(200, 200, 200, 255);
        if (rCtx.graphActive) {
            s_setChannel(rCtx, GC_Background);
            rCtx.drawList->AddCircleFilled(rSlot.screenPos, rSlot.dotRadius, col);
            s_setChannel(rCtx, GC_Content);
        } else {
            ImGui::GetWindowDrawList()->AddCircleFilled(rSlot.screenPos, rSlot.dotRadius, col);
        }
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
    return rCtx.graphs[rCtx.currentGraphId].selectedNode == aNodeId;
}
IMNODAL_API Id GetSelectedNode() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.graphActive && "GetSelectedNode must be called inside BeginGraph/EndGraph");
    return rCtx.graphs[rCtx.currentGraphId].selectedNode;
}
IMNODAL_API void SetSelectedNode(Id aNodeId) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.graphActive && "SetSelectedNode must be called inside BeginGraph/EndGraph");
    rCtx.graphs[rCtx.currentGraphId].selectedNode = aNodeId;
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
    return rCtx.graphs[rCtx.currentGraphId].selectedLink == aLinkId;
}
IMNODAL_API Id GetSelectedLink() {
    Context& rCtx = s_getCtx();
    if (rCtx.graphActive) {
        return rCtx.graphs[rCtx.currentGraphId].selectedLink;
    }
    return rCtx.standaloneSelectedLink;
}
IMNODAL_API void SetSelectedLink(Id aLinkId) {
    Context& rCtx = s_getCtx();
    if (rCtx.graphActive) {
        rCtx.graphs[rCtx.currentGraphId].selectedLink = aLinkId;
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
        const ImVec2 d = aOtherPos - arSlot.screenPos;
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        return (len > 0.0f) ? ImVec2(d.x / len, d.y / len) : ImVec2(1.0f, 0.0f);
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
    rLink.fromSlot = aFromSlotId;
    rLink.toSlot   = aToSlotId;
    rLink.graphId  = rCtx.currentGraphId;
    const ImU32 baseColor = (aColor != 0) ? aColor : IM_COL32(220, 220, 220, 230);
    rLink.color    = baseColor;

    rFrom.connected = true;
    rTo.connected   = true;

    // Resolve tangents (InOut slots are dynamic).
    const ImVec2 fromTan = s_resolveTangent(rFrom, rTo.screenPos);
    const ImVec2 toTan   = s_resolveTangent(rTo,   rFrom.screenPos);

    // Hit-test against the bezier.
    ImVec2 p1, p2;
    s_bezierCtrl(rFrom.screenPos, fromTan, rTo.screenPos, toTan, p1, p2);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float canvasScale = rCtx.scale > 0.0f ? rCtx.scale : 1.0f;
    const float hitThreshold = ImMax(aThickness * 2.0f, 6.0f / canvasScale);

    rLink.hovered = s_isMouseOnBezier(rFrom.screenPos, p1, p2, rTo.screenPos, mouse, hitThreshold);
    rLink.clicked = false;
    rLink.doubleClicked = false;

    // Track selection at graph scope if we're inside a graph, otherwise use
    // the context-level standaloneSelectedLink field.
    GraphState* pGraph = rCtx.graphActive ? &rCtx.graphs[rCtx.currentGraphId] : nullptr;
    const bool clickConsumed = pGraph ? pGraph->clickConsumedThisFrame : false;

    const bool canConsume = rLink.hovered && !ImGui::IsAnyItemHovered() && !clickConsumed && rCtx.draggingFromSlot == 0;
    if (canConsume) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            rLink.clicked = true;
            if (pGraph) {
                pGraph->selectedLink = aLinkId;
                pGraph->selectedNode = 0;
                pGraph->clickConsumedThisFrame = true;
            } else {
                rCtx.standaloneSelectedLink = aLinkId;
            }
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            rLink.doubleClicked = true;
            if (pGraph) pGraph->clickConsumedThisFrame = true;
        }
    }
    const Id selId = pGraph ? pGraph->selectedLink : rCtx.standaloneSelectedLink;
    rLink.selected = (selId == aLinkId);

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

IMNODAL_API void EndConnectionCreate() {
    Context& rCtx = s_getCtx();

    // Only the scope that OWNS the drag draws the preview. Without this
    // check, every EndConnectionCreate call (graph + free-window + …) would
    // draw its own copy of the preview link.
    if (s_isDragInCurrentScope(rCtx)) {
        auto itF = rCtx.slots.find(rCtx.draggingFromSlot);
        if (itF != rCtx.slots.end()) {
            SlotState& rFrom = itF->second;

            ImVec2 toPos;
            ImVec2 toTangent;
            if (rCtx.currentHoveredSlot != 0 && rCtx.currentHoveredSlot != rCtx.draggingFromSlot) {
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
            if (rCtx.connAcceptedThisFrame) {
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

    // End the drag when the mouse is released.
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        rCtx.draggingFromSlot = 0;
    }
}

// =====================================================================
// Reroute node (M2)
// =====================================================================
// Minimal 1-in/1-out pass-through node rendered as a single dot. Draggable;
// clicking on either slot starts a new connection drag.

IMNODAL_API bool BeginRerouteNode(Id aNodeId, Id aSlotId, ImVec2* apPos, const NodeSettings& arSettings) {
    NodeSettings reSettings = arSettings;
    // The node itself is transparent — only the slot dot and the hover drag
    // bar are visible.
    reSettings.bodyColor           = IM_COL32(0, 0, 0, 0);
    reSettings.headerColor         = IM_COL32(0, 0, 0, 0);
    reSettings.borderColor         = IM_COL32(0, 0, 0, 0);
    reSettings.selectedBorderColor = IM_COL32(255, 180, 0, 255);
    reSettings.borderThickness     = 0.0f;
    reSettings.rounding            = 0.0f;
    reSettings.drawHoverHandle     = true;

    if (!BeginNode(aNodeId, apPos, reSettings)) {
        return false;
    }

    // Give the node a small rectangular body so there's drag-room around the
    // slot. The slot itself lives in the middle with a dummy "body cell" so
    // its group rect has a sensible size for dot centering.
    SlotSettings dotSettings{};
    dotSettings.dotRadius = 5.0f;

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
