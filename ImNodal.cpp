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

namespace ImNodal {

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

}  // namespace ImNodal
