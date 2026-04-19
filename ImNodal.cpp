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
};

struct GraphState {
    Id       id{0};
    GraphSettings settings{};
    // Splitter channels managed inside BeginGraph/EndGraph
    bool     splitterActive{false};
    int      preBeginChannel{0};
    // Per-frame set of hovered/selected
    Id       selectedNode{0};
    // Draw order / node Z (M1: simple insertion order)
    std::vector<Id> frameNodeOrder;
    // Set by EndNode when a node consumed the left click this frame. EndGraph
    // reads this to decide whether a left click on empty space should clear selection.
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
};

namespace {

static Context* g_currentCtx{nullptr};

// Access the current context with an assert. Every public Begin/End/query
// routes through this so "no context" fails loud instead of corrupting state.
inline Context& s_getCtx() {
    IM_ASSERT(g_currentCtx != nullptr && "ImNodal: no current context. Call CreateContext() + SetCurrentContext() first.");
    return *g_currentCtx;
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
    IM_ASSERT(arCtx.drawList->_Splitter._Current == arCtx.expectedChannel);

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
    arCtx.bgClicked          = false;
    arCtx.bgDoubleClicked    = false;
    arCtx.bgCtxMenuRequested = false;

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
    IM_ASSERT(aId != nullptr);

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
    IM_ASSERT(rCtx.drawList->_Splitter._Current == rCtx.expectedChannel);
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
    IM_ASSERT(arCtx.graphActive);
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
    IM_ASSERT(rCtx.active && "BeginGraph must be called inside BeginCanvas/EndCanvas");
    IM_ASSERT(!rCtx.graphActive && "BeginGraph called twice without EndGraph");
    IM_ASSERT(aGraphId != 0 && "Graph id must be non-zero");

    rCtx.graphActive = true;
    rCtx.currentGraphId = aGraphId;

    GraphState& rGraph = rCtx.graphs[aGraphId];
    rGraph.id = aGraphId;
    rGraph.settings = arSettings;
    rGraph.frameNodeOrder.clear();

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

    // Background click = left click that no node consumed this frame (M1: clears selection).
    const bool lmbClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (lmbClicked && !rGraph.clickConsumedThisFrame && !rCtx.isPanning) {
        rGraph.selectedNode = 0;
    }
    // Reset for next frame
    rGraph.clickConsumedThisFrame = false;

    // Merge channels back into the canvas-expected channel order.
    if (rGraph.splitterActive) {
        pDrawList->ChannelsMerge();
        rGraph.splitterActive = false;
    }
    // Splitter must be back to the canvas's expected channel
    IM_ASSERT(pDrawList->_Splitter._Current == rCtx.expectedChannel);

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
    IM_ASSERT(rCtx.currentNodeId == 0 && "Nested BeginNode is not supported in M1");
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

    // ---- Commit pending slot X on node edges, draw dots ----
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

        const ImU32 col = rSlot.hovered ? IM_COL32(255,255,255,255)
                        : rSlot.connected ? IM_COL32(255,220,0,255)
                        : IM_COL32(200,200,200,255);
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
    IM_ASSERT(rCtx.currentSection == Context::Section_None);
    rCtx.currentSection = Context::Section_Header;
    rCtx.pinModeStack.push_back(PinMode_Inline);
    ImGui::BeginGroup();
    return true;
}
IMNODAL_API void EndHeader() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.currentSection == Context::Section_Header);
    ImGui::EndGroup();
    NodeState& rNode = rCtx.nodes[rCtx.currentNodeId];
    rNode.hasHeader = true;
    rNode.headerScreenRect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    rCtx.pinModeStack.pop_back();
    rCtx.currentSection = Context::Section_None;
}

// Helper: open body column — if another body column was already opened, emit SameLine first.
static bool s_beginBodyColumn(Context& rCtx, Context::Section aSec, PinMode aMode) {
    IM_ASSERT(rCtx.currentNodeId != 0);
    IM_ASSERT(rCtx.currentSection == Context::Section_None);
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
    IM_ASSERT(rCtx.currentSection == aSec);
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
    IM_ASSERT(rCtx.currentSection == Context::Section_None);
    rCtx.currentSection = Context::Section_Footer;
    rCtx.pinModeStack.push_back(PinMode_Inline);
    ImGui::BeginGroup();
    return true;
}
IMNODAL_API void EndFooter() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.currentSection == Context::Section_Footer);
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
    const bool  isInline = (rSlot.pinMode == PinMode_Inline);
    const bool  isOutput = (aRole == SlotRole_Output);

    // Layout rule:
    //  Pinned LeftEdge (input on node edge): indent from the left by dot padding so the label
    //    leaves room for the dot to sit on the node's left border.
    //  Pinned RightEdge (output on node edge): right-align; add padding ON THE RIGHT after the
    //    user widgets — we achieve that by drawing label first and user widgets after.
    //  Inline: dot side depends on role (input → left of label, output → right).
    if (rSlot.pinMode == PinMode_LeftEdge) {
        // Reserve space at the left so the dot sits on the node edge
        ImGui::Dummy(ImVec2(padding, 0.0f));
        ImGui::SameLine(0.0f, 0.0f);
        if (aLabel && aLabel[0] != 0) {
            ImGui::TextUnformatted(aLabel);
            ImGui::SameLine();
        }
    } else if (rSlot.pinMode == PinMode_RightEdge) {
        // For right-edge outputs, we emit label first then widgets, all followed by right padding.
        if (aLabel && aLabel[0] != 0) {
            ImGui::TextUnformatted(aLabel);
            ImGui::SameLine();
        }
    } else {
        // Inline: dot on the natural side
        if (!isOutput) {
            // input → dot left of label, widget right
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

    // Default screenPos depending on mode (X committed later by EndNode for pinned modes).
    if (rSlot.pinMode == PinMode_LeftEdge) {
        rSlot.screenPos = ImVec2(gMin.x, rSlot.pendingY);   // overwritten by EndNode
        rSlot.tangent   = ImVec2(-1.0f, 0.0f);
    } else if (rSlot.pinMode == PinMode_RightEdge) {
        rSlot.screenPos = ImVec2(gMax.x, rSlot.pendingY);
        rSlot.tangent   = ImVec2(1.0f, 0.0f);
    } else {
        // Inline: dot sits on the relevant side of the group
        const float x = isOutput ? (gMax.x - rSlot.dotRadius) : (gMin.x + rSlot.dotRadius);
        rSlot.screenPos = ImVec2(x, rSlot.pendingY);
        rSlot.tangent   = ImVec2(isOutput ? 1.0f : -1.0f, 0.0f);
    }

    // Hover test — circular, with a min screen-pixel radius at high zoom-out.
    const float scale = rCtx.scale;
    const float minHit = (rSlot.graphId != 0)
        ? (rCtx.graphs.count(rSlot.graphId) ? rCtx.graphs[rSlot.graphId].settings.minSlotHitRadiusScreen : 8.0f)
        : 8.0f;
    // Convert minHit (screen px) to canvas-space px (we're in local canvas space here)
    const float hitRadius = ImMax(rSlot.dotRadius, minHit / (scale > 0.0f ? scale : 1.0f));
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImVec2 d = mouse - rSlot.screenPos;
    rSlot.hovered = (d.x * d.x + d.y * d.y) <= (hitRadius * hitRadius);

    // For pinned modes, queue the slot for X commit at EndNode.
    if (rCtx.currentNodeId != 0 && (rSlot.pinMode == PinMode_LeftEdge || rSlot.pinMode == PinMode_RightEdge)) {
        rCtx.nodes[rCtx.currentNodeId].pendingSlots.push_back(rCtx.currentSlotId);
    } else {
        // Inline / standalone: draw the dot now. If we're inside a graph, use the
        // Background channel so the dot sits under contents. Otherwise fall back to
        // the current window's draw list (standalone slot in a plain ImGui window).
        const ImU32 col = rSlot.hovered ? IM_COL32(255,255,255,255)
                        : rSlot.connected ? IM_COL32(255,220,0,255)
                        : IM_COL32(200,200,200,255);
        if (rCtx.graphActive) {
            s_setChannel(rCtx, GC_Background);
            rCtx.drawList->AddCircleFilled(rSlot.screenPos, rSlot.dotRadius, col);
            s_setChannel(rCtx, GC_Content);
        } else {
            ImDrawList* const pDraw = ImGui::GetWindowDrawList();
            pDraw->AddCircleFilled(rSlot.screenPos, rSlot.dotRadius, col);
        }
    }

    // Invisible click-catcher covering the slot's group rect. Registered via
    // imgui_internal's ItemAdd + ButtonBehavior so it doesn't alter layout
    // cursor (SetCursorScreenPos would trigger the ImGui #5548 extend-bounds
    // assert). Drawn AFTER user widgets so ImGui's ActiveId already belongs to
    // any widget the user clicked (DragFloat, etc.); the catcher only absorbs
    // clicks on empty slot area (label text, padding). This stops those clicks
    // from reaching the node-drag logic (IsAnyItemHovered becomes true) and
    // the window-move logic (ActiveId becomes this button on press). In M2 the
    // same capture will start a connection drag.
    {
        ImGuiWindow* const pWindow = ImGui::GetCurrentWindow();
        const ImGuiID hitId = pWindow->GetID("##slot_hit");
        const ImRect hitBB(gMin, gMax);
        ImGui::KeepAliveID(hitId);
        if (ImGui::ItemAdd(hitBB, hitId)) {
            bool slotHovered = false, slotHeld = false;
            ImGui::ButtonBehavior(hitBB, hitId, &slotHovered, &slotHeld,
                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
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
IMNODAL_API bool IsSlotHovered(Id aSlotId) {
    Context& rCtx = s_getCtx();
    auto it = rCtx.slots.find(aSlotId);
    return (it != rCtx.slots.end()) && it->second.hovered;
}

IMNODAL_API bool IsNodeHovered(Id* apoNodeId) {
    Context& rCtx = s_getCtx();
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
    return rCtx.graphs[rCtx.currentGraphId].selectedNode == aNodeId;
}
IMNODAL_API Id GetSelectedNode() {
    Context& rCtx = s_getCtx();
    return rCtx.graphs[rCtx.currentGraphId].selectedNode;
}
IMNODAL_API void SetSelectedNode(Id aNodeId) {
    Context& rCtx = s_getCtx();
    rCtx.graphs[rCtx.currentGraphId].selectedNode = aNodeId;
}
IMNODAL_API bool IsNodeDragging(Id aNodeId) {
    Context& rCtx = s_getCtx();
    return rCtx.draggingNodeId == aNodeId;
}

}  // namespace ImNodal
