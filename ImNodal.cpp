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
    Id parentNode{0};  // 0 = standalone
    Id graphId{0};     // 0 = standalone (outside any BeginGraph)
    ImNodalSlotFlags flags{ImNodalSlotFlags_None};  // snapshot of BeginSlot flags
    // Pivot point at which links anchor on this slot. Computed at EndSlot
    // from the group rect (or pivot override). Hosts that draw their
    // own dot should draw it at this position so the visible dot matches
    // the link endpoint.
    ImVec2 screenPos{};
    ImVec2 tangent{};       // unit vector pointing away from the slot side
    // Hit rect in local-space, captured at EndSlot. Used by the next frame's
    // BeginSlot to set rSlot.hovered EARLY (so IsSlotHovered() works between
    // BeginSlot and EndSlot — typically where the host queries double-clicks)
    // and by EndGraph to draw the interaction hover frame around the slot.
    ImRect lastHitRect{};
    // Custom hitbox set by SetSlotHitbox between Begin/EndSlot. type==None
    // means "use lastHitRect for hover/click tests" (default rectangular
    // behavior). Otherwise the shape drives hit-tests; lastHitRect still
    // holds the AABB (used for ImGui::ItemAdd / culling and the EndGraph
    // hover frame).
    ImNodalShape customHitbox{};
    // Custom slot body shape set by SetSlotBodyShape between Begin/EndSlot.
    // type==None → no paint (legacy "host paints its own dot" behavior).
    // Circle or ConvexPolygon → EndSlot paints fill (ImNodalCol_SlotBody) +
    // outline (ImNodalCol_SlotBorder). Body shape and hitbox are independent
    // — SetSlotBodyShape auto-sets the hitbox, but an explicit SetSlotHitbox
    // call still wins.
    ImNodalShape bodyShape{};
    bool hovered{false};
    bool ctxMenuRequested{false};  // set by EndSlot when right-clicked
    bool connected{false};         // set by Link() — RESET each NewFrame, then set again by Link() AFTER all Begin/EndSlot ran
    bool connectedLastFrame{false}; // snapshot taken in NewFrame BEFORE `connected` is reset — lets EndSlot's body-paint pick a connected-state color despite the 1-frame lag (Link() runs after EndSlot)
};

struct NodeState {
    ImVec2 pos{};   // canvas-space top-left
    ImVec2 size{};  // filled at EndNode
    Id graphId{0};
    bool hovered{false};
    bool selected{false};
    bool dragging{false};
    bool ctxMenuRequested{false};  // right-click on node, set by EndNode
    // Flags snapshot from BeginNode, consumed by EndNode draw + interactions.
    ImNodalNodeFlags flags{ImNodalNodeFlags_None};
    // Last frame's screen-space rect (for GetNodeRect / per-node drawlists)
    ImRect lastScreenRect{};
    // Custom node hitbox set by SetNodeHitbox between Begin/EndNode. type==None
    // means "use the rectangular nodeMin/nodeMax for hover/click tests" (the
    // legacy behavior). Otherwise the shape drives hit-tests.
    ImNodalShape customHitbox{};
    // Custom node body shape set by SetNodeBodyShape between Begin/EndNode.
    // type==None (or Rect) → default rounded rect fill+border. Circle or
    // ConvexPolygon → ImNodal paints the body in that shape using the theme
    // colors (NodeBody for fill, NodeBorder / NodeBorderSelected for outline).
    // Hitbox and body shape are independent — the host may set one without
    // the other, or pass the same shape to both for a clickable polygon.
    ImNodalShape bodyShape{};
    // Max natural width/height of all TOP-LEVEL BeginLayoutHorizontal/Vertical containers emitted
    // inside this node. Used by "fill parent" mode of subsequent toplevel
    // containers to size against their siblings rather than against
    // node.size — avoids the self-referential feedback that would bake the
    // current padding into the next frame's Spring fill.
    float currentMaxToplevelNatWidth{0.0f};
    float currentMaxToplevelNatHeight{0.0f};
    float lastFrameMaxToplevelNatWidth{0.0f};
    float lastFrameMaxToplevelNatHeight{0.0f};
    // Custom color set by ImNodal::SetNodeColor between BeginNode/EndNode
    // (host's accent / header color). 0 = no custom color. Reset every frame
    // at BeginNode. Read by the minimap and any future feature that wants
    // to highlight nodes with a host-supplied color.
    ImU32 color{0};
};

// =====================================================================
// Layout primitives — H/V containers + Spring (state)
// =====================================================================
// Active stack frame. One entry per open BeginLayoutHorizontal/BeginLayoutVertical in the current node.
struct LayoutContainer {
    ImGuiID id{0};
    bool isHorizontal{true};
    // Snapshot taken at Begin* — used by Spring to know the parent target
    // and by End* to compute the natural size.
    ImVec2 startCursorScreen{};
    ImVec2 targetSize{};       // resolved at Begin (>0 forced, 0 natural, <0 fill parent)
    float consumedAlongAxis{0.0f};  // sum of Spring fills emitted so far this frame
    float springsTotalWeight{0.0f}; // sum of Spring weights opened so far this frame
    // Number of sub-containers / Springs already opened in this container.
    // Used by horizontal containers to emit a SameLine(0,0) before each
    // non-first child so they stack horizontally.
    int childCount{0};
};

// Per-id persistent slot (survives across frames). Stores the natural size
// (= sum of non-Spring children's widths/heights) and the sum of Spring
// weights measured at the previous EndLayoutHorizontal/Vertical — used by LayoutSpring this frame
// to compute its share of the gap.
struct LayoutSlot {
    float lastNatWidth{0.0f};
    float lastNatHeight{0.0f};
    float lastSpringsTotalWeight{0.0f};
};

struct LinkState {
    Id fromSlot{0};
    Id toSlot{0};
    Id graphId{0};
    ImU32 color{0};
    float thickness{3.0f};
    // Polyline emitted this frame in screen-space (filled by primitives between
    // BeginLink/EndLink, drawn in one pass at EndLink). Reused by FlowLink and
    // box-select. Generic over the link shape (cubic bezier, Manhattan, custom).
    std::vector<ImVec2> cachedPath;
    bool pathCached{false};
    // Per-frame interaction state (computed by Link(), reset at NewFrame).
    bool hovered{false};
    bool clicked{false};
    bool doubleClicked{false};
    bool selected{false};
    bool ctxMenuRequested{false};
};

// =====================================================================
// GraphState — per-(canvas, graph) interactivity state
// =====================================================================
// Holds EVERYTHING graph-side: selection, hover-this-frame, drag, the
// connection-create / delete / shortcut state machines, the box-select drag
// tracking, the layout primitive stack, and per-frame "current open" ids
// (node/slot inside the graph). Lifetime: one instance per (canvas, graph)
// pair, lookup via Context::graphs map.
struct GraphState {
    Id id{0};
    GraphSettings settings{};
    // Splitter channels managed inside BeginGraph/EndGraph
    bool splitterActive{false};
    int preBeginChannel{0};
    // Multi-selection: a SET of node ids and link ids. Last-clicked id is
    // tracked so the legacy GetSelectedNode/Link can return a stable "first"
    // element (the one the user touched most recently).
    std::unordered_set<Id> selectedNodes;
    std::unordered_set<Id> selectedLinks;
    Id lastSelectedNode{0};
    Id lastSelectedLink{0};
    // Snapshot of the previous frame's selection — used to fire HasSelectionChanged.
    std::unordered_set<Id> prevSelectedNodes;
    std::unordered_set<Id> prevSelectedLinks;
    bool selectionChangedThisFrame{false};
    // Draw order / node Z (M1: simple insertion order)
    std::vector<Id> frameNodeOrder;
    // Slots emitted this frame INSIDE this graph. Their dots are drawn at
    // EndGraph (after Link() has had a chance to set rSlot.connected) so the
    // accent color reflects the up-to-date connection state.
    std::vector<Id> frameSlotOrder;
    // Set by EndNode or Link() when they consumed the left click this frame.
    // EndGraph reads this to decide whether a left click on empty space should
    // clear selection.
    bool clickConsumedThisFrame{false};
    // Snapshot taken when the box-select becomes active. Live mode rebuilds
    // the selection every frame as (snapshot ∪ overlapping items), so the
    // multi-select modifier acts as "additive" against this fixed base —
    // dragging the box does not erase pre-existing selections.
    std::unordered_set<Id> boxSelectBaseNodes;
    std::unordered_set<Id> boxSelectBaseLinks;
    bool boxSelectBaseCaptured{false};

    // -----------------------------
    // Per-frame "currently open" ids (between Begin/End*)
    // -----------------------------
    Id currentNodeId{0};
    Id currentSlotId{0};
    // Cursor screen pos captured right after BeginSlot's ImGui::BeginGroup.
    // EndSlot compares against the live cursor to detect "host emitted nothing"
    // and substitutes a min-size Dummy so the slot still has a hit area.
    ImVec2 currentSlotBeginCursor{};

    // -----------------------------
    // Active drag target (node being dragged, 0 = none)
    // -----------------------------
    Id draggingNodeId{0};
    ImVec2 dragStartNodePos{};
    ImVec2 dragStartMouseCanvas{};

    // -----------------------------
    // Connection creation state machine (M2)
    // -----------------------------
    Id draggingFromSlot{0};                    // Non-zero → user is dragging a link from this slot
    Id currentHoveredSlot{0};                  // Slot the mouse is currently on this frame
    Id currentHoveredNode{0};                  // Node the mouse is currently on this frame
    Id currentHoveredLink{0};                  // Link the mouse is currently on this frame
    bool connAcceptedThisFrame{false};         // AcceptNewLink called this frame
    bool connNewNodeAcceptedThisFrame{false};  // AcceptNewNodeFromSlot called this frame
    ImU32 connAcceptColor{0};                  // Color user passed to AcceptNewLink/NewNode
    const char* connRejectReason{nullptr};     // RejectNewLink reason (pointer assumed stable for the frame)
    bool connCommitThisFrame{false};           // Set when mouse released AND accepted this frame
    bool connNewNodeCommitThisFrame{false};    // Set when drop-on-empty committed this frame

    // -----------------------------
    // Context-menu requests (right-click on a specific item this frame).
    // Reset every NewFrame; populated by EndNode / EndSlot / Link.
    // -----------------------------
    Id ctxMenuNodeId{0};
    Id ctxMenuSlotId{0};
    Id ctxMenuLinkId{0};

    // -----------------------------
    // Delete state machine
    // -----------------------------
    bool deleteScopeOpen{false};
    std::deque<Id> pendingDeleteLinks;
    std::deque<Id> pendingDeleteNodes;
    Id currentDeleteCandidate{0};  // last id returned by QueryDeletedLink/Node
    int currentDeleteKind{0};      // 0 = none, 1 = link, 2 = node
    // Snapshot of accepted ids in this BeginDelete/EndDelete scope — used to
    // remove them from the selection at EndDelete (so the host doesn't have to).
    std::vector<Id> acceptedDeleteLinks;
    std::vector<Id> acceptedDeleteNodes;

    // -----------------------------
    // Shortcut state machine (Ctrl+C/V/X/D/A)
    // -----------------------------
    bool shortcutScopeOpen{false};
    bool shortcutCopyFired{false};
    bool shortcutPasteFired{false};
    bool shortcutCutFired{false};
    bool shortcutDuplicateFired{false};
    bool shortcutSelectAllFired{false};
    // Snapshot taken when a shortcut fires — host can read it via
    // GetActionContextNodes/Links the same frame.
    std::vector<Id> actionContextNodes;
    std::vector<Id> actionContextLinks;

    // -----------------------------
    // BeginLayoutHorizontal/Vertical/Group + LayoutSpring layout primitives
    // -----------------------------
    // Active container stack (one entry per open BeginLayoutHorizontal/Vertical
    // at the current moment). Empty between EndNode and the next BeginNode.
    std::vector<LayoutContainer> layoutStack;
    // Persistent natural-size cache, keyed by ImGui scope ID. LayoutSpring at
    // frame N reads this to compute its fill ; EndLayoutHorizontal/Vertical
    // writes it for next frame.
    std::unordered_map<ImGuiID, LayoutSlot> layoutSlots;

    // -----------------------------
    // Box-select state (started by left-drag from empty canvas)
    // -----------------------------
    // Two phases: pendingBgClick after mouse-down on empty canvas, then
    // boxSelectActive once the drag distance crosses the threshold. On
    // release, either commit the box (if active) or treat it as a plain
    // bg-click (if not). Coordinates are LOCAL-SPACE (i.e. canvas-space
    // when inside BeginCanvas) — same space used by node lastNodeRect and
    // link cached endpoints.
    bool pendingBgClick{false};
    ImVec2 pendingBgClickPos{};        // local-space (at the moment of the click)
    ImVec2 pendingBgClickPosScreen{};  // screen-space (for the drag threshold)
    bool boxSelectActive{false};
    ImVec2 boxSelectStart{};  // local-space

    // GC: last frame this graph was visited by BeginGraph. NewFrame purges
    // graphs unseen for more than IMNODAL_GC_FRAMES.
    int lastSeenFrame{-1};
};

// =====================================================================
// CanvasState — per-canvas view + canvas-level interactivity state
// =====================================================================
// Holds EVERYTHING canvas-side: view transform (origin/scale/viewRect),
// pan, background interactions (bgClick / ctxMenu), suspend nesting,
// draw-list state, mouse / viewport / window backups, minimap state, the
// BeginLink/EndLink scope (which can be used standalone, outside any graph),
// and the per-slot/-node staging (alignment, hitbox). Lifetime: one instance
// per canvas id, lookup via Context::canvases map.
struct CanvasState {
    ImGuiID id{0};               // hash of the canvas string id
    bool active{false};          // true between BeginCanvas/EndCanvas

    CanvasSettings settings;

    // Widget geometry (screen space)
    ImVec2 widgetPos{};
    ImVec2 widgetSize{};
    ImRect widgetRect{};

    // View: screen = canvas * scale + origin + widgetPos
    ImVec2 origin{};  // Translation in screen pixels, relative to widgetPos
    float scale{1.0f};
    float invScale{1.0f};
    ImVec2 viewTransformPos{};  // origin + widgetPos — recomputed on change

    // Visible canvas-space rect (valid while inside local space)
    ImRect viewRect{};

    // Draw list state snapshot (for local-space vertex transform)
    ImDrawList* drawList{nullptr};
    int expectedChannel{0};
    int cmdBufferSize{0};
    int startVertexIdx{0};
    float lastFringeScale{1.0f};

    // Pan
    bool isPanning{false};
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
    // MiniMap state
    // -----------------------------
    // Set by ShowMiniMap each frame ; gates graph hit-tests so the minimap
    // behaves like a floating window over the graph (cursor over the minimap
    // doesn't reach nodes/links/slots/pan/box-select beneath it).
    ImRect lastMiniMapRect{};
    bool   minimapHovered{false};
    bool   minimapActive{false};   // true while LMB is held inside the minimap (drag-to-recenter)

    // -----------------------------
    // BeginLink / EndLink scope (custom-link primitives)
    // -----------------------------
    // Open between BeginLink and EndLink. Primitives (LinkLineSegment,
    // LinkBezierSegment, LinkPolyline) push into the active LinkState's
    // cachedPath and OR-accumulate currentLinkHovered. EndLink resolves the
    // final color and emits a single AddPolyline. Scoped to canvas (not graph)
    // since standalone links can be drawn outside any BeginGraph.
    Id currentLinkId{0};
    ImU32 currentLinkBaseColor{0};
    float currentLinkThickness{0.0f};
    float currentLinkHitThreshold{0.0f};
    bool currentLinkHovered{false};
    ImVec2 currentLinkFromPos{};
    ImVec2 currentLinkToPos{};
    ImVec2 currentLinkFromTangent{};
    ImVec2 currentLinkToTangent{};
    ImDrawList* currentLinkDrawList{nullptr};

    // -----------------------------
    // Slot pivot override (current slot only) — thedmd-style.
    // pivot.Min = group.Min + group.Size * slotAlignment
    // pivot.Max = pivot.Min + slotSize
    // The link endpoint anchors to the center of the pivot rect (or to
    // the rect itself when slotSize is non-zero — point in the (0,0) case).
    // -----------------------------
    ImVec2 slotAlignment{0.5f, 0.5f};   // CENTER of group rect by default
    ImVec2 slotSize{0.0f, 0.0f};        // zero -> pivot is a point
    ImVec2 slotPivotOffset{0.0f, 0.0f}; // extra px offset added to screenPos
    // Absolute overrides set by SetSlotAnchor / SetSlotTangent — bypass the
    // group-rect-relative alignment+size+offset math when set. Useful for
    // custom polygonal slots whose link endpoint sits on a point of the
    // shape that has no simple alignment expression. Reset at EndSlot.
    bool slotAnchorSet{false};
    ImVec2 slotAnchorPos{0.0f, 0.0f};
    bool slotTangentSet{false};
    ImVec2 slotTangentDir{0.0f, 0.0f};

    // Staging hitboxes filled by SetSlotHitbox / SetNodeHitbox between
    // Begin* and End*. Read + applied at End* into the matching state, then
    // reset to "no override" so the next slot/node starts fresh.
    ImNodalShape stagingSlotHitbox{};
    ImNodalShape stagingNodeHitbox{};
    // Staging body shapes filled by SetNodeBodyShape / SetSlotBodyShape
    // between Begin*/End*. Same lifecycle as the hitbox stagings.
    ImNodalShape stagingNodeBodyShape{};
    ImNodalShape stagingSlotBodyShape{};

    // Global "selected link" for standalone (outside-of-graph) link queries.
    // Lives at canvas level (not graph) since a standalone link has no graph.
    Id standaloneSelectedLink{0};

    // GC: last frame this canvas was visited by BeginCanvas. NewFrame purges
    // canvases unseen for more than IMNODAL_GC_FRAMES.
    int lastSeenFrame{-1};
};

// =====================================================================
// GraphKey — composite key for the Context::graphs map
// =====================================================================
// A graph is identified by the (canvas, graphId) pair: the same graph id
// can be used in multiple canvases and they stay independent. Combined hash
// via ImHashData over both fields.
struct GraphKey {
    ImGuiID canvas;
    Id graph;
    bool operator==(const GraphKey& arOther) const {
        return canvas == arOther.canvas && graph == arOther.graph;
    }
};

struct GraphKeyHash {
    size_t operator()(const GraphKey& arKey) const {
        return (size_t)ImHashData(&arKey, sizeof(arKey), 0);
    }
};

// =====================================================================
// Context (opaque publicly — declared in ImNodal.h)
// =====================================================================

// Style modification stack entries (one per Push call ; popped in reverse).
struct ColorMod {
    ImNodalCol idx;
    ImU32 prev;
};
struct VarMod {
    ImNodalStyleVar idx;
    bool isVec2;
    float prevF;
    ImVec2 prevV2;
};

// =====================================================================
// Context — slim global state (one instance per application, ImGui-style)
// =====================================================================
// Holds only what is truly global: appearance (Style + push/pop stacks),
// the frame counter for NewFrame idempotence, the entity maps
// (nodes/slots/links — keyed globally with a `graphId` scoping field), and
// the per-canvas / per-(canvas,graph) state maps. The current canvas/graph
// is tracked via push/pop stacks (set by BeginCanvas/BeginGraph) with a
// "last touched" fallback so queries still work after EndCanvas/EndGraph
// the same frame.
struct Context {
    Style style;                       // global appearance state
    std::vector<ColorMod> colorStack;  // PushStyleColor history
    std::vector<VarMod> varStack;      // PushStyleVar history

    // Entity maps — global ID space, scoped per-graph via a `graphId` field
    // on each entry (kept global to minimize disruption to existing host code
    // that assumes globally unique ids).
    std::unordered_map<Id, NodeState> nodes;
    std::unordered_map<Id, SlotState> slots;
    std::unordered_map<Id, LinkState> links;

    // Per-canvas state (key = ImHashStr(aCanvasId)).
    std::unordered_map<ImGuiID, CanvasState> canvases;
    // Per-(canvas, graph) state (key = { canvasKey, graphId }).
    std::unordered_map<GraphKey, GraphState, GraphKeyHash> graphs;

    // Current canvas / graph pointers — set by Begin/End* push, kept in
    // sync via the stacks. After End*, the pointer points to the last
    // touched entry so queries called outside of any scope still work
    // (typical: post-EndCanvas queries like IsCanvasBackgroundClicked).
    CanvasState* pCanvas{nullptr};
    GraphState*  pGraph{nullptr};
    std::vector<CanvasState*> canvasStack;
    std::vector<GraphState*>  graphStack;

    // Set by ImNodal::NewFrame() to ImGui::GetFrameCount() so repeated calls
    // within the same frame are idempotent.
    int lastFrameReset{-1};

    // Accessors — assert there IS a current/last-touched canvas/graph. Use
    // in the bulk of internal code; the rare "may not be set" callsites
    // check pCanvas/pGraph against nullptr explicitly. Const overloads are
    // there for helpers that take `const Context& arCtx` (read-only views).
    inline CanvasState& Canvas() {
        IM_ASSERT(pCanvas != nullptr && "ImNodal: no current canvas. Call BeginCanvas() first (or SetCurrentEditor with a known canvas id).");
        return *pCanvas;
    }
    inline const CanvasState& Canvas() const {
        IM_ASSERT(pCanvas != nullptr && "ImNodal: no current canvas. Call BeginCanvas() first (or SetCurrentEditor with a known canvas id).");
        return *pCanvas;
    }
    inline GraphState& Graph() {
        IM_ASSERT(pGraph != nullptr && "ImNodal: no current graph. Call BeginGraph() first (or SetCurrentEditor with a known canvas+graph id).");
        return *pGraph;
    }
    inline const GraphState& Graph() const {
        IM_ASSERT(pGraph != nullptr && "ImNodal: no current graph. Call BeginGraph() first (or SetCurrentEditor with a known canvas+graph id).");
        return *pGraph;
    }
};

namespace {

// Number of frames an unseen CanvasState / GraphState entry survives before
// NewFrame purges it from the Context maps. 60 = 1s at 60 fps, same window
// ImGui uses for inactive windows.
static constexpr int IMNODAL_GC_FRAMES = 60;

static Context* g_currentCtx{nullptr};

// Access the current context with an assert. Every public Begin/End/query
// routes through this so "no context" fails loud instead of corrupting state.
inline Context& s_getCtx() {
    IM_ASSERT(g_currentCtx != nullptr && "ImNodal: no current context. Call CreateContext() + SetCurrentContext() first.");
    return *g_currentCtx;
}

// Reset all per-frame state. Invoked by the public ImNodal::NewFrame().
// Idempotent within a frame: the lastFrameReset guard means extra calls in the
// same frame are no-ops. Called BEFORE any BeginCanvas/BeginGraph this frame,
// so we iterate the persistent maps directly (no current canvas/graph yet).
static void s_doNewFrame(Context& arCtx) {
    const int frame = ImGui::GetFrameCount();
    if (arCtx.lastFrameReset == frame)
        return;
    arCtx.lastFrameReset = frame;

    // Safety: if the mouse has been up for strictly MORE than one frame and a
    // drag state is still set on ANY graph, the user forgot a matching
    // EndConnectionCreate (or their BeginConnectionCreate was skipped for
    // scope reasons). Clear it. We must NOT clear on the release frame itself:
    // EndConnectionCreate needs IsMouseReleased==true this frame to commit.
    const bool mouseUp = !ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool onReleaseFrame = ImGui::IsMouseReleased(ImGuiMouseButton_Left);

    // Reset per-frame state on every graph (the host may juggle multiple
    // graphs across canvases — each has its own scope-local frame state).
    for (auto& kv : arCtx.graphs) {
        GraphState& g = kv.second;
        g.currentHoveredSlot = 0;
        g.currentHoveredNode = 0;
        g.currentHoveredLink = 0;
        g.connAcceptedThisFrame = false;
        g.connNewNodeAcceptedThisFrame = false;
        g.connAcceptColor = 0;
        g.connRejectReason = nullptr;
        g.connCommitThisFrame = false;
        g.connNewNodeCommitThisFrame = false;
        g.ctxMenuNodeId = 0;
        g.ctxMenuSlotId = 0;
        g.ctxMenuLinkId = 0;
        g.shortcutCopyFired = false;
        g.shortcutPasteFired = false;
        g.shortcutCutFired = false;
        g.shortcutDuplicateFired = false;
        g.shortcutSelectAllFired = false;
        // Layout primitive : drop any leftover container from a previous frame
        // where the host forgot an EndLayoutHorizontal/Vertical. Persistent
        // layoutSlots cache survives across frames.
        g.layoutStack.clear();
        g.selectionChangedThisFrame = false;
        // Drag stale-clear (per-graph).
        if (g.draggingFromSlot != 0 && mouseUp && !onReleaseFrame) {
            g.draggingFromSlot = 0;
        }
    }

    // Reset per-frame state on every canvas (slot pivot, staging hitboxes —
    // these are canvas-scoped because slots may live outside of any graph).
    for (auto& kv : arCtx.canvases) {
        CanvasState& c = kv.second;
        c.slotAlignment = ImVec2(0.5f, 0.5f);
        c.slotSize = ImVec2(0.0f, 0.0f);
        c.slotPivotOffset = ImVec2(0.0f, 0.0f);
        c.slotAnchorSet = false;
        c.slotAnchorPos = ImVec2(0.0f, 0.0f);
        c.slotTangentSet = false;
        c.slotTangentDir = ImVec2(0.0f, 0.0f);
        // Drop any staging hitbox / body shape a host forgot to consume by
        // closing its matching Begin/End scope last frame. ImNodalShape{} == None.
        c.stagingSlotHitbox = ImNodalShape{};
        c.stagingNodeHitbox = ImNodalShape{};
        c.stagingSlotBodyShape = ImNodalShape{};
        c.stagingNodeBodyShape = ImNodalShape{};
    }

    for (auto& kv : arCtx.slots) {
        // Snapshot connected BEFORE resetting it — EndSlot's body-paint uses
        // connectedLastFrame because Link() (which sets `connected`) runs
        // AFTER all Begin/EndSlot pairs of the current frame.
        kv.second.connectedLastFrame = kv.second.connected;
        kv.second.connected = false;
        kv.second.ctxMenuRequested = false;
    }
    for (auto& kv : arCtx.links) {
        kv.second.hovered = false;
        kv.second.clicked = false;
        kv.second.doubleClicked = false;
        kv.second.ctxMenuRequested = false;
        kv.second.pathCached = false;
        kv.second.cachedPath.clear();
    }
    for (auto& kv : arCtx.nodes) {
        kv.second.ctxMenuRequested = false;
    }

    // GC pass: drop CanvasState / GraphState entries unseen for > K frames.
    // Same window ImGui uses for inactive windows. Hosts juggling dynamic
    // canvas/graph ids (one per document, per scratchpad...) get their stale
    // entries reclaimed automatically.
    for (auto it = arCtx.canvases.begin(); it != arCtx.canvases.end(); ) {
        if (frame - it->second.lastSeenFrame > IMNODAL_GC_FRAMES) {
            // If lastTouched still points at this entry, invalidate the pointer
            // BEFORE erasing (otherwise pCanvas dangles).
            if (arCtx.pCanvas == &it->second) arCtx.pCanvas = nullptr;
            it = arCtx.canvases.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = arCtx.graphs.begin(); it != arCtx.graphs.end(); ) {
        if (frame - it->second.lastSeenFrame > IMNODAL_GC_FRAMES) {
            if (arCtx.pGraph == &it->second) arCtx.pGraph = nullptr;
            it = arCtx.graphs.erase(it);
        } else {
            ++it;
        }
    }
}

// True if the current drag (if any) originated from a slot whose scope
// matches the currently open scope — graph id when inside BeginGraph, or 0
// for standalone slots. Used to gate connection-creation queries/commits so
// a drag started in one scope doesn't fire handlers in another.
static bool s_isDragInCurrentScope(const Context& arCtx) {
    if (arCtx.pGraph == nullptr)
        return false;
    const GraphState& rGraph = *arCtx.pGraph;
    if (rGraph.draggingFromSlot == 0)
        return false;
    auto it = arCtx.slots.find(rGraph.draggingFromSlot);
    if (it == arCtx.slots.end())
        return false;
    return it->second.graphId == rGraph.id;
}

inline ImVec2 s_selectPositive(const ImVec2& arA, const ImVec2& arB) {
    return ImVec2(arA.x > 0.0f ? arA.x : arB.x, arA.y > 0.0f ? arA.y : arB.y);
}

// ---------------------------------------------------------------------
// Coordinate helpers
// ---------------------------------------------------------------------
inline ImVec2 s_canvasToScreen(const Context& arCtx, const ImVec2& aP) {
    return aP * arCtx.Canvas().scale + arCtx.Canvas().viewTransformPos;
}
inline ImVec2 s_screenToCanvas(const Context& arCtx, const ImVec2& aP) {
    return (aP - arCtx.Canvas().viewTransformPos) * arCtx.Canvas().invScale;
}

inline ImRect s_calcViewRect(const Context& arCtx) {
    ImRect r;
    r.Min = ImVec2(-arCtx.Canvas().origin.x, -arCtx.Canvas().origin.y) * arCtx.Canvas().invScale;
    r.Max = (arCtx.Canvas().widgetSize - arCtx.Canvas().origin) * arCtx.Canvas().invScale;
    return r;
}

inline void s_updateViewTransformPos(Context& arCtx) {
    arCtx.Canvas().viewTransformPos = arCtx.Canvas().origin + arCtx.Canvas().widgetPos;
}

// ---------------------------------------------------------------------
// Local-space enter/leave — the core of the zoom trick.
// While in local space, ImGui draws in canvas coordinates (unscaled).
// At leave, we bake vertices back into screen space applying scale.
// ---------------------------------------------------------------------

static void s_saveInputState(Context& arCtx) {
    const auto& rIo = ImGui::GetIO();
    arCtx.Canvas().mousePosBackup = rIo.MousePos;
    arCtx.Canvas().mousePosPrevBackup = rIo.MousePosPrev;
    for (int i = 0; i < IM_ARRAYSIZE(arCtx.Canvas().mouseClickedPosBackup); ++i) {
        arCtx.Canvas().mouseClickedPosBackup[i] = rIo.MouseClickedPos[i];
    }
}

static void s_restoreInputState(Context& arCtx) {
    auto& rIo = ImGui::GetIO();
    rIo.MousePos = arCtx.Canvas().mousePosBackup;
    rIo.MousePosPrev = arCtx.Canvas().mousePosPrevBackup;
    for (int i = 0; i < IM_ARRAYSIZE(arCtx.Canvas().mouseClickedPosBackup); ++i) {
        rIo.MouseClickedPos[i] = arCtx.Canvas().mouseClickedPosBackup[i];
    }
}

static void s_saveViewportState(Context& arCtx) {
    auto* const pWindow = ImGui::GetCurrentWindow();
    auto* const pViewport = ImGui::GetWindowViewport();
    arCtx.Canvas().windowPosBackup = pWindow->Pos;
    arCtx.Canvas().viewportPosBackup = pViewport->Pos;
    arCtx.Canvas().viewportSizeBackup = pViewport->Size;
    arCtx.Canvas().viewportWorkPosBackup = pViewport->WorkPos;
    arCtx.Canvas().viewportWorkSizeBackup = pViewport->WorkSize;
}

static void s_restoreViewportState(Context& arCtx) {
    auto* const pWindow = ImGui::GetCurrentWindow();
    auto* const pViewport = ImGui::GetWindowViewport();
    pWindow->Pos = arCtx.Canvas().windowPosBackup;
    pViewport->Pos = arCtx.Canvas().viewportPosBackup;
    pViewport->Size = arCtx.Canvas().viewportSizeBackup;
    pViewport->WorkPos = arCtx.Canvas().viewportWorkPosBackup;
    pViewport->WorkSize = arCtx.Canvas().viewportWorkSizeBackup;
}

static void s_enterLocalSpace(Context& arCtx) {
    // Resolve clipped clip rect BEFORE opening a new command.
    ImGui::PushClipRect(arCtx.Canvas().widgetPos, arCtx.Canvas().widgetPos + arCtx.Canvas().widgetSize, true);
    auto clippedClipRect = arCtx.Canvas().drawList->_ClipRectStack.back();
    ImGui::PopClipRect();

    auto* const pWindow = ImGui::GetCurrentWindow();
    auto* const pViewport = ImGui::GetWindowViewport();
    pWindow->Pos = ImVec2(0.0f, 0.0f);

    // Transform viewport to local space
    ImVec2 vpMin = arCtx.Canvas().viewportPosBackup;
    ImVec2 vpMax = arCtx.Canvas().viewportPosBackup + arCtx.Canvas().viewportSizeBackup;
    vpMin = (vpMin - arCtx.Canvas().viewTransformPos) * arCtx.Canvas().invScale;
    vpMax = (vpMax - arCtx.Canvas().viewTransformPos) * arCtx.Canvas().invScale;
    pViewport->Pos = vpMin;
    pViewport->Size = vpMax - vpMin;
    pViewport->WorkPos = arCtx.Canvas().viewportWorkPosBackup * arCtx.Canvas().invScale;
    pViewport->WorkSize = arCtx.Canvas().viewportWorkSizeBackup * arCtx.Canvas().invScale;

    // Clip rect in local space
    clippedClipRect.x = (clippedClipRect.x - arCtx.Canvas().viewTransformPos.x) * arCtx.Canvas().invScale;
    clippedClipRect.y = (clippedClipRect.y - arCtx.Canvas().viewTransformPos.y) * arCtx.Canvas().invScale;
    clippedClipRect.z = (clippedClipRect.z - arCtx.Canvas().viewTransformPos.x) * arCtx.Canvas().invScale;
    clippedClipRect.w = (clippedClipRect.w - arCtx.Canvas().viewTransformPos.y) * arCtx.Canvas().invScale;
    ImGui::PushClipRect(ImVec2(clippedClipRect.x, clippedClipRect.y), ImVec2(clippedClipRect.z, clippedClipRect.w), false);

    // Capture the cmd buffer + vertex index AFTER the PushClipRect above.
    // When pre-canvas widgets exist on the same ImGui window, that PushClipRect
    // opens a new draw cmd (previous cmd has elements with a different clip
    // rect, so _OnChangedClipRect calls AddDrawCmd). leaveLocalSpace's
    // clip-rect remap loop must start AFTER the pre-canvas command, otherwise
    // it remaps that pre-canvas cmd's ClipRect to local space and clips the
    // pre-canvas widgets off-screen. Capturing here puts cmdBufferSize on
    // the new canvas command regardless of whether PushClipRect created one
    // or merely updated the current (empty) one.
    arCtx.Canvas().cmdBufferSize = ImMax(arCtx.Canvas().drawList->CmdBuffer.Size - 1, 0);
    arCtx.Canvas().startVertexIdx = arCtx.Canvas().drawList->_VtxCurrentIdx + arCtx.Canvas().drawList->_CmdHeader.VtxOffset;

    // Mouse in local space
    auto& rIo = ImGui::GetIO();
    rIo.MousePos = (arCtx.Canvas().mousePosBackup - arCtx.Canvas().viewTransformPos) * arCtx.Canvas().invScale;
    rIo.MousePosPrev = (arCtx.Canvas().mousePosPrevBackup - arCtx.Canvas().viewTransformPos) * arCtx.Canvas().invScale;
    for (int i = 0; i < IM_ARRAYSIZE(arCtx.Canvas().mouseClickedPosBackup); ++i) {
        rIo.MouseClickedPos[i] = (arCtx.Canvas().mouseClickedPosBackup[i] - arCtx.Canvas().viewTransformPos) * arCtx.Canvas().invScale;
    }

    arCtx.Canvas().viewRect = s_calcViewRect(arCtx);

    // Scale AA thickness so strokes render crisp at any zoom
    arCtx.Canvas().lastFringeScale = arCtx.Canvas().drawList->_FringeScale;
    arCtx.Canvas().drawList->_FringeScale *= arCtx.Canvas().invScale;
}

static void s_leaveLocalSpace(Context& arCtx) {
    IM_ASSERT(arCtx.Canvas().drawList->_Splitter._Current == arCtx.Canvas().expectedChannel && "Unbalanced channel splitter on leave local space");

    // Bake new vertices: canvas-space -> screen-space
    auto* pVtx = arCtx.Canvas().drawList->VtxBuffer.Data + arCtx.Canvas().startVertexIdx;
    auto* pVtxEnd = arCtx.Canvas().drawList->VtxBuffer.Data + arCtx.Canvas().drawList->_VtxCurrentIdx + arCtx.Canvas().drawList->_CmdHeader.VtxOffset;

    if (arCtx.Canvas().scale != 1.0f) {
        while (pVtx < pVtxEnd) {
            pVtx->pos.x = pVtx->pos.x * arCtx.Canvas().scale + arCtx.Canvas().viewTransformPos.x;
            pVtx->pos.y = pVtx->pos.y * arCtx.Canvas().scale + arCtx.Canvas().viewTransformPos.y;
            ++pVtx;
        }
        for (int i = arCtx.Canvas().cmdBufferSize; i < arCtx.Canvas().drawList->CmdBuffer.size(); ++i) {
            auto& rCmd = arCtx.Canvas().drawList->CmdBuffer[i];
            rCmd.ClipRect.x = rCmd.ClipRect.x * arCtx.Canvas().scale + arCtx.Canvas().viewTransformPos.x;
            rCmd.ClipRect.y = rCmd.ClipRect.y * arCtx.Canvas().scale + arCtx.Canvas().viewTransformPos.y;
            rCmd.ClipRect.z = rCmd.ClipRect.z * arCtx.Canvas().scale + arCtx.Canvas().viewTransformPos.x;
            rCmd.ClipRect.w = rCmd.ClipRect.w * arCtx.Canvas().scale + arCtx.Canvas().viewTransformPos.y;
        }
    } else {
        while (pVtx < pVtxEnd) {
            pVtx->pos.x += arCtx.Canvas().viewTransformPos.x;
            pVtx->pos.y += arCtx.Canvas().viewTransformPos.y;
            ++pVtx;
        }
        for (int i = arCtx.Canvas().cmdBufferSize; i < arCtx.Canvas().drawList->CmdBuffer.size(); ++i) {
            auto& rCmd = arCtx.Canvas().drawList->CmdBuffer[i];
            rCmd.ClipRect.x += arCtx.Canvas().viewTransformPos.x;
            rCmd.ClipRect.y += arCtx.Canvas().viewTransformPos.y;
            rCmd.ClipRect.z += arCtx.Canvas().viewTransformPos.x;
            rCmd.ClipRect.w += arCtx.Canvas().viewTransformPos.y;
        }
    }

    arCtx.Canvas().drawList->_FringeScale = arCtx.Canvas().lastFringeScale;

    ImGui::PopClipRect();
    s_restoreInputState(arCtx);
    s_restoreViewportState(arCtx);
}

// ---------------------------------------------------------------------
// Interaction: pan + zoom. Called from BeginCanvas while in local space.
// ---------------------------------------------------------------------

static void s_managePan(Context& arCtx) {
    const auto btn = arCtx.Canvas().settings.panButton;

    // arCtx.Canvas().hovered = strict canvas gate (set at BeginCanvas). Once panning
    // is engaged, the drag continues even if the mouse leaves the canvas
    // (normal UX) — the second branch handles release.
    if ((arCtx.Canvas().isPanning || arCtx.Canvas().hovered) && ImGui::IsMouseDragging(btn, 0.0f)) {
        if (!arCtx.Canvas().isPanning) {
            arCtx.Canvas().isPanning = true;
            arCtx.Canvas().panStartOrigin = arCtx.Canvas().origin;
        }
        const ImVec2 delta = ImGui::GetMouseDragDelta(btn, 0.0f) * arCtx.Canvas().scale;
        SetCanvasView(arCtx.Canvas().panStartOrigin + delta, arCtx.Canvas().scale);
    } else if (arCtx.Canvas().isPanning && !ImGui::IsMouseDown(btn)) {
        arCtx.Canvas().isPanning = false;
    }
}

static void s_manageZoom(Context& arCtx) {
    const auto& rIo = ImGui::GetIO();
    const float wheel = rIo.MouseWheel;
    const bool resetKey = (arCtx.Canvas().settings.resetZoomKey != ImGuiKey_None) && ImGui::IsKeyPressed(arCtx.Canvas().settings.resetZoomKey);

    if (wheel == 0.0f && !resetKey) {
        return;
    }

    if (resetKey) {
        SetCanvasView(arCtx.Canvas().widgetSize * 0.5f, 1.0f);
        return;
    }

    const float newScale = ImClamp(arCtx.Canvas().scale + wheel * arCtx.Canvas().settings.zoomStep, arCtx.Canvas().settings.zoomMin, arCtx.Canvas().settings.zoomMax);
    if (newScale == arCtx.Canvas().scale) {
        return;
    }

    // Zoom anchored on cursor: same canvas point stays under mouse before/after.
    // mousePosBackup is cursor in screen space (saved before entering local space).
    const ImVec2 mouseCanvas = (arCtx.Canvas().mousePosBackup - arCtx.Canvas().viewTransformPos) * arCtx.Canvas().invScale;
    const ImVec2 newOrigin = arCtx.Canvas().mousePosBackup - arCtx.Canvas().widgetPos - mouseCanvas * newScale;
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
    // Canvas-hovered gate : zoom/pan only fire when the mouse is on THIS
    // canvas (not on a sibling panel, another window, or another canvas).
    if (!arCtx.Canvas().hovered) {
        return;
    }
    s_manageZoom(arCtx);
    s_managePan(arCtx);
}

}  // namespace

// =====================================================================
// Style — defaults + Push/Pop API
// =====================================================================

Style::Style() {
    // Defaults : sane dark-theme values matching what the lib used hardcoded
    // before the style refactor. Tweak at runtime via GetStyle() or via the
    // Push/Pop helpers.
    Colors[ImNodalCol_GridLine] = IM_COL32(200, 200, 200, 40);
    Colors[ImNodalCol_GridSubLine] = IM_COL32(200, 200, 200, 10);
    Colors[ImNodalCol_NodeBody] = IM_COL32(50, 50, 50, 230);
    Colors[ImNodalCol_NodeBorder] = IM_COL32(80, 80, 80, 255);
    Colors[ImNodalCol_NodeBorderSelected] = IM_COL32(255, 180, 0, 255);
    Colors[ImNodalCol_NodeHoverHandle] = IM_COL32(255, 255, 255, 120);
    Colors[ImNodalCol_Link] = IM_COL32(220, 220, 220, 230);
    Colors[ImNodalCol_LinkHovered] = IM_COL32(255, 255, 255, 240);
    Colors[ImNodalCol_LinkSelected] = IM_COL32(255, 180, 0, 230);
    Colors[ImNodalCol_BoxSelectFill] = IM_COL32(120, 200, 255, 40);
    Colors[ImNodalCol_BoxSelectBorder] = IM_COL32(120, 200, 255, 200);
    Colors[ImNodalCol_LinkPreviewIdle] = IM_COL32(220, 220, 220, 200);
    Colors[ImNodalCol_LinkPreviewAccept] = IM_COL32(120, 255, 120, 230);
    Colors[ImNodalCol_LinkPreviewReject] = IM_COL32(255, 80, 80, 230);
    Colors[ImNodalCol_FlowDot] = IM_COL32(255, 220, 120, 255);
    Colors[ImNodalCol_MiniMapBg] = IM_COL32(20, 20, 20, 200);
    Colors[ImNodalCol_MiniMapBorder] = IM_COL32(180, 180, 180, 200);
    Colors[ImNodalCol_MiniMapNode] = IM_COL32(140, 140, 140, 255);
    Colors[ImNodalCol_MiniMapViewport] = IM_COL32(255, 200, 80, 220);
    Colors[ImNodalCol_SlotBody] = IM_COL32(80, 80, 80, 230);
    Colors[ImNodalCol_SlotBorder] = IM_COL32(180, 180, 180, 255);
    Colors[ImNodalCol_SlotHovered] = IM_COL32(255, 220, 120, 255);
    Colors[ImNodalCol_SlotConnected] = IM_COL32(120, 200, 255, 255);

    NodeRounding = 4.0f;
    NodeBorderThickness = 1.5f;
    NodeBodyPadding = 6.0f;
    NodeHoverHandleHeight = 4.0f;
    SlotMinSize = ImVec2(12.0f, 12.0f);
    SlotBorderThickness = 1.0f;
    LinkThickness = 3.0f;
    GridSize = ImVec2(50.0f, 50.0f);
    GridSubdivs = ImVec2(5.0f, 5.0f);
}

IMNODAL_API Style& GetStyle() {
    return s_getCtx().style;
}

IMNODAL_API ImU32 GetStyleColorU32(ImNodalCol aIdx) {
    if (aIdx < 0 || aIdx >= ImNodalCol_COUNT)
        return 0;
    return s_getCtx().style.Colors[aIdx];
}

IMNODAL_API float GetStyleVarFloat(ImNodalStyleVar aIdx) {
    Style& s = s_getCtx().style;
    switch (aIdx) {
        case ImNodalStyleVar_NodeRounding: return s.NodeRounding;
        case ImNodalStyleVar_NodeBorderThickness: return s.NodeBorderThickness;
        case ImNodalStyleVar_NodeBodyPadding: return s.NodeBodyPadding;
        case ImNodalStyleVar_NodeHoverHandleHeight: return s.NodeHoverHandleHeight;
        case ImNodalStyleVar_LinkThickness: return s.LinkThickness;
        case ImNodalStyleVar_SlotBorderThickness: return s.SlotBorderThickness;
        default: return 0.0f;
    }
    return 0.0f;
}

IMNODAL_API ImVec2 GetStyleVarVec2(ImNodalStyleVar aIdx) {
    Style& s = s_getCtx().style;
    switch (aIdx) {
        case ImNodalStyleVar_GridSize: return s.GridSize;
        case ImNodalStyleVar_GridSubdivs: return s.GridSubdivs;
        case ImNodalStyleVar_SlotMinSize: return s.SlotMinSize;
        default: return ImVec2(0.0f, 0.0f);
    }
}

IMNODAL_API bool PushStyleColor(ImNodalCol aIdx, ImU32 aCol) {
    if (aIdx < 0 || aIdx >= ImNodalCol_COUNT)
        return false;
    Context& rCtx = s_getCtx();
    rCtx.colorStack.push_back({aIdx, rCtx.style.Colors[aIdx]});
    rCtx.style.Colors[aIdx] = aCol;
    return true;
}

IMNODAL_API void PopStyleColor(int aCount) {
    Context& rCtx = s_getCtx();
    while (aCount-- > 0 && !rCtx.colorStack.empty()) {
        const auto& m = rCtx.colorStack.back();
        rCtx.style.Colors[m.idx] = m.prev;
        rCtx.colorStack.pop_back();
    }
}

namespace {
// Reads/writes a float style var by index. Returns reference for r/w.
inline float* s_styleVarFloatPtr(Style& s, ImNodalStyleVar aIdx) {
    switch (aIdx) {
        case ImNodalStyleVar_NodeRounding: return &s.NodeRounding;
        case ImNodalStyleVar_NodeBorderThickness: return &s.NodeBorderThickness;
        case ImNodalStyleVar_NodeBodyPadding: return &s.NodeBodyPadding;
        case ImNodalStyleVar_NodeHoverHandleHeight: return &s.NodeHoverHandleHeight;
        case ImNodalStyleVar_LinkThickness: return &s.LinkThickness;
        case ImNodalStyleVar_SlotBorderThickness: return &s.SlotBorderThickness;
        default: return nullptr;
    }
}
inline ImVec2* s_styleVarVec2Ptr(Style& s, ImNodalStyleVar aIdx) {
    switch (aIdx) {
        case ImNodalStyleVar_GridSize: return &s.GridSize;
        case ImNodalStyleVar_GridSubdivs: return &s.GridSubdivs;
        case ImNodalStyleVar_SlotMinSize: return &s.SlotMinSize;
        default: return nullptr;
    }
}
}  // namespace

IMNODAL_API bool PushStyleVar(ImNodalStyleVar aIdx, float aVal) {
    Context& rCtx = s_getCtx();
    float* p = s_styleVarFloatPtr(rCtx.style, aIdx);
    if (p == nullptr)
        return false;
    VarMod m;
    m.idx = aIdx;
    m.isVec2 = false;
    m.prevF = *p;
    *p = aVal;
    rCtx.varStack.push_back(m);
    return true;
}

IMNODAL_API bool PushStyleVar(ImNodalStyleVar aIdx, const ImVec2& aVal) {
    Context& rCtx = s_getCtx();
    ImVec2* p = s_styleVarVec2Ptr(rCtx.style, aIdx);
    if (p == nullptr)
        return false;
    VarMod m;
    m.idx = aIdx;
    m.isVec2 = true;
    m.prevV2 = *p;
    *p = aVal;
    rCtx.varStack.push_back(m);
    return true;
}

IMNODAL_API void PopStyleVar(int aCount) {
    Context& rCtx = s_getCtx();
    while (aCount-- > 0 && !rCtx.varStack.empty()) {
        const auto& m = rCtx.varStack.back();
        if (m.isVec2) {
            ImVec2* p = s_styleVarVec2Ptr(rCtx.style, m.idx);
            if (p)
                *p = m.prevV2;
        } else {
            float* p = s_styleVarFloatPtr(rCtx.style, m.idx);
            if (p)
                *p = m.prevF;
        }
        rCtx.varStack.pop_back();
    }
}

// =====================================================================
// Style editors
// =====================================================================

IMNODAL_API const char* GetStyleColorName(ImNodalCol aIdx) {
    switch (aIdx) {
        case ImNodalCol_GridLine: return "GridLine";
        case ImNodalCol_GridSubLine: return "GridSubLine";
        case ImNodalCol_NodeBody: return "NodeBody";
        case ImNodalCol_NodeBorder: return "NodeBorder";
        case ImNodalCol_NodeBorderSelected: return "NodeBorderSelected";
        case ImNodalCol_NodeHoverHandle: return "NodeHoverHandle";
        case ImNodalCol_Link: return "Link";
        case ImNodalCol_LinkHovered: return "LinkHovered";
        case ImNodalCol_LinkSelected: return "LinkSelected";
        case ImNodalCol_BoxSelectFill: return "BoxSelectFill";
        case ImNodalCol_BoxSelectBorder: return "BoxSelectBorder";
        case ImNodalCol_LinkPreviewIdle: return "LinkPreviewIdle";
        case ImNodalCol_LinkPreviewAccept: return "LinkPreviewAccept";
        case ImNodalCol_LinkPreviewReject: return "LinkPreviewReject";
        case ImNodalCol_FlowDot: return "FlowDot";
        case ImNodalCol_MiniMapBg: return "MiniMapBg";
        case ImNodalCol_MiniMapBorder: return "MiniMapBorder";
        case ImNodalCol_MiniMapNode: return "MiniMapNode";
        case ImNodalCol_MiniMapViewport: return "MiniMapViewport";
        case ImNodalCol_SlotBody: return "SlotBody";
        case ImNodalCol_SlotBorder: return "SlotBorder";
        case ImNodalCol_SlotHovered: return "SlotHovered";
        case ImNodalCol_SlotConnected: return "SlotConnected";
        default: return "Unknown";
    }
}

IMNODAL_API const char* GetStyleVarName(ImNodalStyleVar aIdx) {
    switch (aIdx) {
        case ImNodalStyleVar_NodeRounding: return "NodeRounding";
        case ImNodalStyleVar_NodeBorderThickness: return "NodeBorderThickness";
        case ImNodalStyleVar_NodeBodyPadding: return "NodeBodyPadding";
        case ImNodalStyleVar_NodeHoverHandleHeight: return "NodeHoverHandleHeight";
        case ImNodalStyleVar_SlotMinSize: return "SlotMinSize";
        case ImNodalStyleVar_LinkThickness: return "LinkThickness";
        case ImNodalStyleVar_SlotBorderThickness: return "SlotBorderThickness";
        case ImNodalStyleVar_GridSize: return "GridSize";
        case ImNodalStyleVar_GridSubdivs: return "GridSubdivs";
        default: return "Unknown";
    }
}

IMNODAL_API void ShowStyleColorsEditor() {
    Style& s = s_getCtx().style;
    if (ImGui::SmallButton("Reset all colors")) {
        // Default-construct a fresh Style and copy only the color slots back.
        const Style def{};
        for (int i = 0; i < ImNodalCol_COUNT; ++i)
            s.Colors[i] = def.Colors[i];
    }
    ImGui::Separator();
    ImGui::PushID("ImNodal_StyleColors");
    for (int i = 0; i < ImNodalCol_COUNT; ++i) {
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(s.Colors[i]);
        ImGui::PushID(i);
        if (ImGui::ColorEdit4(
                GetStyleColorName(static_cast<ImNodalCol>(i)), &c.x, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) {
            s.Colors[i] = ImGui::ColorConvertFloat4ToU32(c);
        }
        ImGui::PopID();
    }
    ImGui::PopID();
}

IMNODAL_API void ShowStyleVarsEditor() {
    Style& s = s_getCtx().style;
    if (ImGui::SmallButton("Reset all vars")) {
        const Style def{};
        s.NodeRounding = def.NodeRounding;
        s.NodeBorderThickness = def.NodeBorderThickness;
        s.NodeBodyPadding = def.NodeBodyPadding;
        s.NodeHoverHandleHeight = def.NodeHoverHandleHeight;
        s.SlotMinSize = def.SlotMinSize;
        s.LinkThickness = def.LinkThickness;
        s.SlotBorderThickness = def.SlotBorderThickness;
        s.GridSize = def.GridSize;
        s.GridSubdivs = def.GridSubdivs;
    }
    ImGui::Separator();
    ImGui::PushID("ImNodal_StyleVars");
    // Reasonable per-var bounds so DragFloat sliders don't go out of useful range.
    struct FloatRange {
        float lo;
        float hi;
        float step;
    };
    auto floatRange = [](ImNodalStyleVar v) -> FloatRange {
        switch (v) {
            case ImNodalStyleVar_NodeRounding: return {0.0f, 20.0f, 0.1f};
            case ImNodalStyleVar_NodeBorderThickness: return {0.0f, 8.0f, 0.1f};
            case ImNodalStyleVar_NodeBodyPadding: return {0.0f, 32.0f, 0.5f};
            case ImNodalStyleVar_NodeHoverHandleHeight: return {0.0f, 32.0f, 0.5f};
            case ImNodalStyleVar_LinkThickness: return {0.5f, 16.0f, 0.1f};
            case ImNodalStyleVar_SlotBorderThickness: return {0.5f, 8.0f, 0.1f};
            default: return {0.0f, 100.0f, 0.5f};
        }
    };
    for (int i = 0; i < ImNodalStyleVar_COUNT; ++i) {
        const auto v = static_cast<ImNodalStyleVar>(i);
        const char* name = GetStyleVarName(v);
        ImGui::PushID(i);
        if (float* fp = s_styleVarFloatPtr(s, v)) {
            const FloatRange r = floatRange(v);
            ImGui::DragFloat(name, fp, r.step, r.lo, r.hi, "%.2f");
        } else if (ImVec2* vp = s_styleVarVec2Ptr(s, v)) {
            if (v == ImNodalStyleVar_SlotMinSize) {
                // Pixel-ish small range — used as the empty-slot Dummy size.
                ImGui::DragFloat2(name, &vp->x, 0.5f, 0.0f, 64.0f, "%.1f");
            } else {
                // Grid metrics are integer-ish; keep step at 1 and clamp to >=1
                // so the user can't dial them to zero (would divide-by-zero in
                // grid drawing).
                ImGui::DragFloat2(name, &vp->x, 1.0f, 1.0f, 1024.0f, "%.0f");
            }
        }
        ImGui::PopID();
    }
    ImGui::PopID();
}

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
    IM_ASSERT(pTarget->canvasStack.empty() && pTarget->graphStack.empty()
              && "DestroyContext called while a Begin/End scope is still open");
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

IMNODAL_API void SetCurrentEditor(const char* aCanvasId, Id aGraphId) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(aCanvasId != nullptr && "SetCurrentEditor: canvas id must be non-null");
    const ImGuiID canvasKey = ImHashStr(aCanvasId);
    auto cit = rCtx.canvases.find(canvasKey);
    IM_ASSERT(cit != rCtx.canvases.end() && "SetCurrentEditor: unknown canvas id (no recent BeginCanvas with this name)");
    rCtx.pCanvas = &cit->second;
    if (aGraphId != 0) {
        auto git = rCtx.graphs.find(GraphKey{canvasKey, aGraphId});
        IM_ASSERT(git != rCtx.graphs.end() && "SetCurrentEditor: unknown graph id under this canvas");
        rCtx.pGraph = &git->second;
    } else {
        rCtx.pGraph = nullptr;
    }
}

IMNODAL_API bool HasEditor(const char* aCanvasId, Id aGraphId) {
    Context& rCtx = s_getCtx();
    if (aCanvasId == nullptr) return false;
    const ImGuiID canvasKey = ImHashStr(aCanvasId);
    if (rCtx.canvases.find(canvasKey) == rCtx.canvases.end()) return false;
    if (aGraphId == 0) return true;  // canvas exists, graph not requested
    return rCtx.graphs.find(GraphKey{canvasKey, aGraphId}) != rCtx.graphs.end();
}

IMNODAL_API void NewFrame() {
    s_doNewFrame(s_getCtx());
}

// -----------------------------
// Debug
// -----------------------------
IMNODAL_API bool DebugCheckVersion(const char* aVersion, size_t aSettingsSize) {
    const bool vers = (aVersion != nullptr) && (std::strcmp(aVersion, IMNODAL_VERSION) == 0);
    const bool sz = (aSettingsSize == sizeof(CanvasSettings));
    IM_ASSERT(vers && "ImNodal version mismatch");
    IM_ASSERT(sz && "ImNodal CanvasSettings size mismatch");
    return vers && sz;
}

IMNODAL_API bool BeginCanvas(const char* aId, const ImVec2& aSize, const CanvasSettings& arSettings) {
    Context& rCtx = s_getCtx();
    // Mirror ImGui's "Forgot to call ImGui::NewFrame()" assert. ImNodal
    // relies on NewFrame() to clear per-frame state (hovered slot /
    // link ids, conn flags, ctxMenu requests, link hovered / clicked /
    // doubleClicked, stale drag state). Skipping it leaves stale ids
    // around and silently breaks gestures (background double-click /
    // right-click after a link has been hovered, drag-from-slot create
    // node entirely). Catching the omission here, before any of that
    // state is consulted, gives a clear stack trace instead of cryptic
    // UI symptoms.
    IM_ASSERT(rCtx.lastFrameReset == ImGui::GetFrameCount()
              && "Forgot to call ImNodal::NewFrame() this frame ? "
                 "Call it once per frame on the current context, "
                 "right after ImGui::NewFrame().");
    IM_ASSERT(aId != nullptr && "BeginCanvas: id must be non-null");

    // Look up (or create) the CanvasState for this canvas id. Push on the
    // stack and set the current pointer — all subsequent Canvas() accesses
    // resolve to this state.
    const ImGuiID canvasKey = ImHashStr(aId);
    CanvasState& rCanvas = rCtx.canvases[canvasKey];
    IM_ASSERT(rCanvas.active == false && "BeginCanvas called twice without EndCanvas for this canvas id");
    rCanvas.id = canvasKey;
    rCanvas.lastSeenFrame = ImGui::GetFrameCount();
    rCtx.pCanvas = &rCanvas;
    rCtx.canvasStack.push_back(&rCanvas);

    rCtx.Canvas().settings = arSettings;

    rCtx.Canvas().widgetPos = ImGui::GetCursorScreenPos();
    rCtx.Canvas().widgetSize = s_selectPositive(aSize, ImGui::GetContentRegionAvail());
    rCtx.Canvas().widgetRect = ImRect(rCtx.Canvas().widgetPos, rCtx.Canvas().widgetPos + rCtx.Canvas().widgetSize);
    rCtx.Canvas().drawList = ImGui::GetWindowDrawList();

    // Auto-center the view on the first successful Begin
    if (!rCtx.Canvas().viewInitialized) {
        rCtx.Canvas().origin = rCtx.Canvas().widgetSize * 0.5f;
        rCtx.Canvas().scale = 1.0f;
        rCtx.Canvas().invScale = 1.0f;
        rCtx.Canvas().viewInitialized = true;
    }
    s_updateViewTransformPos(rCtx);

    const ImGuiID id = ImGui::GetID(aId);
    if (ImGui::IsClippedEx(rCtx.Canvas().widgetRect, id)) {
        // Clipped — caller won't pair this with EndCanvas, so we need to
        // unwind the canvas stack push ourselves. pCanvas keeps pointing
        // to this CanvasState (as a "last touched" fallback) so a query
        // right after still resolves, even though the canvas was clipped.
        IM_ASSERT(!rCtx.canvasStack.empty() && rCtx.canvasStack.back() == rCtx.pCanvas);
        rCtx.canvasStack.pop_back();
        return false;
    }

    rCtx.Canvas().expectedChannel = rCtx.Canvas().drawList->_Splitter._Current;
    rCtx.Canvas().windowCursorMaxBackup = ImGui::GetCurrentWindow()->DC.CursorMaxPos;

    // Block window-move ONLY when the mouse is inside the canvas area. This
    // preserves titlebar drag (mouse on titlebar → no NoMove → move works),
    // while clicks/drags inside the canvas never start a window move.
    // ImGui's window-move decision runs at EndFrame and reads the current
    // flags, so setting it here takes effect on this frame's mouse-press.
    const bool mouseInWidget = ImGui::IsMouseHoveringRect(rCtx.Canvas().widgetPos, rCtx.Canvas().widgetPos + rCtx.Canvas().widgetSize);
    if (mouseInWidget) {
        ImGui::GetCurrentWindow()->Flags |= ImGuiWindowFlags_NoMove;
    }

    // Compute canvas-hovered EARLY so it's available throughout the scope
    // (s_manageInteractions, BeginNode, BeginSlot, LinkLineSegment, ...).
    // Strict gate : ImGui window currently hovered AND mouse is geometrically
    // inside our widget rect. Without this, interactions of one canvas would
    // fire on top of another canvas / a side panel / another window.
    //
    // ImGuiHoveredFlags_AllowWhenBlockedByActiveItem : without this flag,
    // IsWindowHovered() returns false as soon as an item is held active
    // (e.g. while dragging a slot to start a connection). That broke target
    // slot detection : the dragged source had ActiveId, IsWindowHovered
    // returned false, rCtx.Canvas().hovered became false, the destination slot's
    // mouseOnSlot was gated off, and currentHoveredSlot stayed 0 → no link
    // could be completed.
    //
    // Also factor in the minimap state (set by the previous frame's
    // ShowMiniMap call) so the cursor over the minimap doesn't trigger any
    // graph hit-test or pan, and a drag started inside the minimap blocks
    // the graph until released.
    rCtx.Canvas().hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
                   && mouseInWidget
                   && !rCtx.Canvas().minimapHovered && !rCtx.Canvas().minimapActive;

    s_saveInputState(rCtx);
    s_saveViewportState(rCtx);

    s_enterLocalSpace(rCtx);
    rCtx.Canvas().active = true;

    // Push a canvas-scoped ID so any user widget emitted between BeginCanvas
    // and EndCanvas lives in its own ID namespace (avoids clashes with widgets
    // outside the canvas, and with widgets in another canvas in the same frame).
    ImGui::PushID(aId);

    // Mouse is now in local space. Interactions run here.
    s_manageInteractions(rCtx);

    if (!(rCtx.Canvas().settings.flags & ImNodalCanvasFlags_NoGrid)) {
        DrawCanvasGrid();
    }

    // Reset ImGui cursor so user drawing starts at canvas origin (0,0).
    ImGui::SetCursorScreenPos(ImVec2(0.0f, 0.0f));

    return true;
}

IMNODAL_API void EndCanvas() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Canvas().active == true && "EndCanvas without matching BeginCanvas");
    IM_ASSERT(rCtx.Canvas().drawList->_Splitter._Current == rCtx.Canvas().expectedChannel && "Unbalanced channel splitter inside canvas scope");
    IM_ASSERT(rCtx.Canvas().suspendCounter == 0 && "Unmatched SuspendCanvas/ResumeCanvas");

    // Match the PushID(aId) at BeginCanvas.
    ImGui::PopID();

    s_leaveLocalSpace(rCtx);
    ImGui::GetCurrentWindow()->DC.CursorMaxPos = rCtx.Canvas().windowCursorMaxBackup;

    // Background interaction flags: rCtx.Canvas().hovered was set early at BeginCanvas
    // (strict canvas-hovered gate, used by all interactions inside the scope).
    // We re-read it here for the bg-click logic — value is stable across the
    // frame since neither IsWindowHovered nor mouse pos changes in between.
    // "onEmpty" = truly on the background : no hovered ImGui item (covers
    // nodes/slots that emit ItemAdd) AND no hovered LINK (links are drawn
    // through DrawList without ItemAdd, so IsAnyItemHovered ignores them —
    // without this check, double-clicking a link would also fire
    // bgDoubleClicked and trigger the host's "fit-to-view" action).
    // currentHoveredLink lives in GraphState. EndCanvas can legally be called
    // for canvas-only use (no BeginGraph in this canvas), so we read it via
    // the pGraph pointer with a null guard rather than the Graph() accessor.
    const Id hoveredLinkId = (rCtx.pGraph != nullptr) ? rCtx.pGraph->currentHoveredLink : (Id)0;
    const bool onEmpty = rCtx.Canvas().hovered && !ImGui::IsAnyItemHovered() && hoveredLinkId == 0 && !rCtx.Canvas().isPanning;
    if (onEmpty) {
        rCtx.Canvas().bgClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        rCtx.Canvas().bgDoubleClicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        rCtx.Canvas().bgCtxMenuRequested = ImGui::IsMouseClicked(rCtx.Canvas().settings.contextMenuButton);
    } else {
        rCtx.Canvas().bgClicked = false;
        rCtx.Canvas().bgDoubleClicked = false;
        rCtx.Canvas().bgCtxMenuRequested = false;
    }

    // Advance ImGui layout cursor.
    ImGui::SetCursorScreenPos(rCtx.Canvas().widgetPos);
    ImGui::Dummy(rCtx.Canvas().widgetSize);

    rCtx.Canvas().active = false;

    // Pop the canvas stack. pCanvas keeps pointing to the popped state so
    // queries called right after EndCanvas (same frame) still see this
    // canvas as the "last touched" one.
    IM_ASSERT(!rCtx.canvasStack.empty() && rCtx.canvasStack.back() == rCtx.pCanvas && "EndCanvas: canvas stack desync");
    rCtx.canvasStack.pop_back();
    if (!rCtx.canvasStack.empty()) {
        rCtx.pCanvas = rCtx.canvasStack.back();
    }
}

// -----------------------------
// Queries
// -----------------------------
IMNODAL_API bool IsCanvasHovered() {
    return s_getCtx().Canvas().hovered;
}
IMNODAL_API bool IsCanvasBackgroundClicked() {
    return s_getCtx().Canvas().bgClicked;
}
IMNODAL_API bool IsCanvasBackgroundDoubleClicked() {
    return s_getCtx().Canvas().bgDoubleClicked;
}
IMNODAL_API bool IsCanvasContextMenuRequested() {
    return s_getCtx().Canvas().bgCtxMenuRequested;
}
IMNODAL_API bool IsCanvasPanning() {
    return s_getCtx().Canvas().isPanning;
}

// -----------------------------
// View
// -----------------------------
IMNODAL_API ImVec2 GetCanvasOrigin() {
    return s_getCtx().Canvas().origin;
}
IMNODAL_API float GetCanvasScale() {
    return s_getCtx().Canvas().scale;
}

IMNODAL_API void SetCanvasView(const ImVec2& aOrigin, float aScale) {
    Context& rCtx = s_getCtx();
    const bool reenter = rCtx.Canvas().active && rCtx.Canvas().suspendCounter == 0;
    if (reenter) {
        s_leaveLocalSpace(rCtx);
    }
    const bool originChanged = (rCtx.Canvas().origin.x != aOrigin.x) || (rCtx.Canvas().origin.y != aOrigin.y);
    if (originChanged) {
        rCtx.Canvas().origin = aOrigin;
        s_updateViewTransformPos(rCtx);
    }
    if (rCtx.Canvas().scale != aScale) {
        rCtx.Canvas().scale = aScale;
        rCtx.Canvas().invScale = (aScale != 0.0f) ? 1.0f / aScale : 0.0f;
    }
    // Mark the view as initialized so the next BeginCanvas doesn't auto-
    // center back to (widgetSize/2, scale=1) and overwrite this value —
    // typically called by hosts that restore the view from saved state
    // (XML load, undo, etc.) BEFORE the first BeginCanvas of the frame.
    rCtx.Canvas().viewInitialized = true;
    if (reenter) {
        s_enterLocalSpace(rCtx);
    }
}

IMNODAL_API void ResetCanvasView() {
    Context& rCtx = s_getCtx();
    SetCanvasView(rCtx.Canvas().widgetSize * 0.5f, 1.0f);
}

IMNODAL_API void CenterCanvasOn(const ImVec2& aCanvasPos) {
    Context& rCtx = s_getCtx();
    const ImVec2 localCenter = s_screenToCanvas(rCtx, rCtx.Canvas().widgetPos + rCtx.Canvas().widgetSize * 0.5f);
    const ImVec2 localOffset = aCanvasPos - localCenter;
    const ImVec2 screenOffset = localOffset * rCtx.Canvas().scale;
    SetCanvasView(rCtx.Canvas().origin - screenOffset, rCtx.Canvas().scale);
}

IMNODAL_API void ZoomCanvasToRect(const ImVec2& aMin, const ImVec2& aMax, float aMarginRatio) {
    Context& rCtx = s_getCtx();
    ImVec2 size = aMax - aMin;
    if (size.x <= 0.0f || size.y <= 0.0f || rCtx.Canvas().widgetSize.x <= 0.0f || rCtx.Canvas().widgetSize.y <= 0.0f) {
        return;
    }
    // Expand the target rect by margin on every side.
    const float extend = ImMax(size.x, size.y) * aMarginRatio * 0.5f;
    const ImVec2 growMin = aMin - ImVec2(extend, extend);
    const ImVec2 growMax = aMax + ImVec2(extend, extend);
    size = growMax - growMin;

    const float widgetAR = rCtx.Canvas().widgetSize.x / rCtx.Canvas().widgetSize.y;
    const float rectAR = size.x / size.y;

    float newScale;
    ImVec2 newOrigin;
    if (rectAR > widgetAR) {
        newScale = rCtx.Canvas().widgetSize.x / size.x;
        newOrigin = growMin * -newScale;
        newOrigin.y += (rCtx.Canvas().widgetSize.y - size.y * newScale) * 0.5f;
    } else {
        newScale = rCtx.Canvas().widgetSize.y / size.y;
        newOrigin = growMin * -newScale;
        newOrigin.x += (rCtx.Canvas().widgetSize.x - size.x * newScale) * 0.5f;
    }
    newScale = ImClamp(newScale, rCtx.Canvas().settings.zoomMin, rCtx.Canvas().settings.zoomMax);
    SetCanvasView(newOrigin, newScale);
}

// -----------------------------
// Coords
// -----------------------------
IMNODAL_API ImVec2 CanvasToScreen(const ImVec2& aP) {
    return s_canvasToScreen(s_getCtx(), aP);
}
IMNODAL_API ImVec2 ScreenToCanvas(const ImVec2& aP) {
    return s_screenToCanvas(s_getCtx(), aP);
}
IMNODAL_API ImVec2 CanvasToScreenV(const ImVec2& aV) {
    return aV * s_getCtx().Canvas().scale;
}
IMNODAL_API ImVec2 ScreenToCanvasV(const ImVec2& aV) {
    return aV * s_getCtx().Canvas().invScale;
}

// -----------------------------
// Rects
// -----------------------------
IMNODAL_API ImRect GetCanvasRect() {
    return s_getCtx().Canvas().widgetRect;
}
IMNODAL_API ImRect GetCanvasViewRect() {
    return s_getCtx().Canvas().viewRect;
}

// -----------------------------
// Suspend / Resume
// -----------------------------
IMNODAL_API void SuspendCanvas() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Canvas().active && "SuspendCanvas outside of Begin/EndCanvas");
    IM_ASSERT(rCtx.Canvas().drawList->_Splitter._Current == rCtx.Canvas().expectedChannel && "SuspendCanvas: unbalanced channel splitter");
    if (rCtx.Canvas().suspendCounter == 0) {
        s_leaveLocalSpace(rCtx);
    }
    ++rCtx.Canvas().suspendCounter;
}

IMNODAL_API void ResumeCanvas() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Canvas().active && "ResumeCanvas outside of Begin/EndCanvas");
    IM_ASSERT(rCtx.Canvas().suspendCounter > 0 && "Unmatched ResumeCanvas");
    if (--rCtx.Canvas().suspendCounter == 0) {
        s_enterLocalSpace(rCtx);
    }
}

IMNODAL_API bool IsCanvasSuspended() {
    return s_getCtx().Canvas().suspendCounter > 0;
}

// -----------------------------
// Grid
// -----------------------------
IMNODAL_API void DrawCanvasGrid() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Canvas().active && "DrawCanvasGrid outside of Begin/EndCanvas");

    const Style& s = rCtx.style;
    auto* const pDrawList = rCtx.Canvas().drawList;
    const ImVec2 offset = rCtx.Canvas().origin * rCtx.Canvas().invScale;
    const ImVec2 winPos = rCtx.Canvas().viewRect.Min;
    const ImVec2 size = rCtx.Canvas().viewRect.GetSize();
    const ImU32 majorCol = s.Colors[ImNodalCol_GridLine];
    const ImU32 minorCol = s.Colors[ImNodalCol_GridSubLine];
    const ImVec2 gridSize = s.GridSize;
    const ImVec2 gridSubdivs = s.GridSubdivs;

    // Major lines
    for (float x = std::fmod(offset.x, gridSize.x); x < size.x; x += gridSize.x) {
        pDrawList->AddLine(ImVec2(x, 0.0f) + winPos, ImVec2(x, size.y) + winPos, majorCol);
    }
    for (float y = std::fmod(offset.y, gridSize.y); y < size.y; y += gridSize.y) {
        pDrawList->AddLine(ImVec2(0.0f, y) + winPos, ImVec2(size.x, y) + winPos, majorCol);
    }

    // Minor subdivisions
    if (gridSubdivs.x != 0.0f && gridSubdivs.y != 0.0f) {
        const ImVec2 sub = gridSize / gridSubdivs;
        for (float x = std::fmod(offset.x, sub.x); x < size.x; x += sub.x) {
            pDrawList->AddLine(ImVec2(x, 0.0f) + winPos, ImVec2(x, size.y) + winPos, minorCol);
        }
        for (float y = std::fmod(offset.y, sub.y); y < size.y; y += sub.y) {
            pDrawList->AddLine(ImVec2(0.0f, y) + winPos, ImVec2(size.x, y) + winPos, minorCol);
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
    GC_Links = 0,       // links drawn under everything else
    GC_BgFill = 1,      // node body fill
    GC_UserBg = 2,      // user-drawn overlays UNDER the border (host header band, etc.)
    GC_BgBorder = 3,    // node border + hover handle (drawn ON TOP of user header band so border stays visible all around)
    GC_Content = 4,     // user widgets inside nodes
    GC_UserFg = 5,      // user-drawn overlays above content
    GC_Overlay = 6,     // box-select, drag-preview link (topmost)
    GC_Count = 7,
};

// Hash Id(u64) -> ImGuiID(u32). ImGui uses u32 internally; we need to push
// scoped IDs from our u64s safely.
static ImGuiID s_imguiId(Id aId) {
    return ImHashData(&aId, sizeof(aId), 0);
}

// Bring the draw list to a given graph channel (asserts graph is active).
static void s_setChannel(Context& arCtx, int aChannel) {
    IM_ASSERT((arCtx.pGraph != nullptr) && "s_setChannel requires an active graph (BeginGraph scope)");
    arCtx.Canvas().drawList->ChannelsSetCurrent(aChannel);
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
        const bool wasOnly = arGraph.selectedNodes.size() == 1 && *arGraph.selectedNodes.begin() == aId;
        const bool noLinks = arGraph.selectedLinks.empty();
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
        const bool wasOnly = arGraph.selectedLinks.size() == 1 && *arGraph.selectedLinks.begin() == aId;
        const bool noNodes = arGraph.selectedNodes.empty();
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
    if (arGraph.settings.flags & ImNodalGraphFlags_NoMultiSelect)
        return false;
    return ImGui::IsKeyDown(arGraph.settings.multiSelectKey);
}

// -----------------------------
// Custom hitbox helpers
// -----------------------------

// Validate a polygon is convex AND wound in a consistent direction (CCW or
// CW). Walks edges once, computing the cross product of consecutive edge
// vectors; all cross-products must share the same sign. Degenerate
// polygons (count < 3) are rejected. Linear time, no allocation, and only
// runs at SetSlotHitbox/SetNodeHitbox call time — never per hit-test.
static bool s_isConvexPolygon(const ImVec2* apPoints, int aCount) {
    if (apPoints == nullptr || aCount < 3)
        return false;
    float dirSign = 0.0f;  // 0 = unset
    for (int i = 0; i < aCount; ++i) {
        const ImVec2& a = apPoints[i];
        const ImVec2& b = apPoints[(i + 1) % aCount];
        const ImVec2& c = apPoints[(i + 2) % aCount];
        const float ex1 = b.x - a.x;
        const float ey1 = b.y - a.y;
        const float ex2 = c.x - b.x;
        const float ey2 = c.y - b.y;
        const float cross = ex1 * ey2 - ey1 * ex2;
        if (std::fabs(cross) < 1e-6f)
            continue;  // collinear — does not break convexity
        if (dirSign == 0.0f) {
            dirSign = (cross > 0.0f) ? 1.0f : -1.0f;
        } else if ((cross > 0.0f) != (dirSign > 0.0f)) {
            return false;
        }
    }
    return true;
}

// Hit-test an arbitrary shape against a screen-space mouse position.
//   None   -> falls back on the AABB the caller provides (same as legacy).
//   Rect   -> standard AABB containment.
//   Circle -> radial distance test.
//   ConvexPolygon -> all cross(edge_i, mouse - p_i) share the same sign.
static bool s_hitTestShape(const ImNodalShape& arShape, const ImVec2& aMouse, const ImRect& arFallbackRect) {
    switch (arShape.type) {
        case ImNodalHitShape_Rect: {
            return aMouse.x >= arShape.rect.Min.x && aMouse.x <= arShape.rect.Max.x &&
                   aMouse.y >= arShape.rect.Min.y && aMouse.y <= arShape.rect.Max.y;
        }
        case ImNodalHitShape_Circle: {
            const float dx = aMouse.x - arShape.center.x;
            const float dy = aMouse.y - arShape.center.y;
            return (dx * dx + dy * dy) <= (arShape.radius * arShape.radius);
        }
        case ImNodalHitShape_ConvexPolygon: {
            if (arShape.polygonPoints == nullptr || arShape.polygonCount < 3)
                return false;
            float dirSign = 0.0f;
            for (int i = 0; i < arShape.polygonCount; ++i) {
                const ImVec2& a = arShape.polygonPoints[i];
                const ImVec2& b = arShape.polygonPoints[(i + 1) % arShape.polygonCount];
                const float ex = b.x - a.x;
                const float ey = b.y - a.y;
                const float vx = aMouse.x - a.x;
                const float vy = aMouse.y - a.y;
                const float cross = ex * vy - ey * vx;
                if (std::fabs(cross) < 1e-6f)
                    continue;
                if (dirSign == 0.0f) {
                    dirSign = (cross > 0.0f) ? 1.0f : -1.0f;
                } else if ((cross > 0.0f) != (dirSign > 0.0f)) {
                    return false;
                }
            }
            return true;
        }
        case ImNodalHitShape_None:
        default:
            return aMouse.x >= arFallbackRect.Min.x && aMouse.x <= arFallbackRect.Max.x &&
                   aMouse.y >= arFallbackRect.Min.y && aMouse.y <= arFallbackRect.Max.y;
    }
}

// AABB englobing a shape — used for ImGui::ItemAdd / culling. Falls back
// to the supplied rect when the shape has no override.
static ImRect s_hitTestAABB(const ImNodalShape& arShape, const ImRect& arFallbackRect) {
    switch (arShape.type) {
        case ImNodalHitShape_Rect:
            return arShape.rect;
        case ImNodalHitShape_Circle: {
            const ImVec2 r(arShape.radius, arShape.radius);
            return ImRect(arShape.center - r, arShape.center + r);
        }
        case ImNodalHitShape_ConvexPolygon: {
            if (arShape.polygonPoints == nullptr || arShape.polygonCount < 1)
                return arFallbackRect;
            ImVec2 lo = arShape.polygonPoints[0];
            ImVec2 hi = lo;
            for (int i = 1; i < arShape.polygonCount; ++i) {
                lo.x = ImMin(lo.x, arShape.polygonPoints[i].x);
                lo.y = ImMin(lo.y, arShape.polygonPoints[i].y);
                hi.x = ImMax(hi.x, arShape.polygonPoints[i].x);
                hi.y = ImMax(hi.y, arShape.polygonPoints[i].y);
            }
            return ImRect(lo, hi);
        }
        case ImNodalHitShape_None:
        default:
            return arFallbackRect;
    }
}

// -----------------------------
// Box-select hit helpers (intersection-based)
// -----------------------------

// Slab method : true if the line segment (aA, aB) intersects the AABB
// (aMin, aMax). Returns true if either endpoint is inside, OR if the segment
// crosses the box. Used by the box-select to test bezier samples chord by
// chord — catches cases where the curve dips into the box between samples.
static bool s_segmentIntersectsRect(const ImVec2& aA, const ImVec2& aB, const ImVec2& aMin, const ImVec2& aMax) {
    const auto inside = [&](const ImVec2& p) { return p.x >= aMin.x && p.x <= aMax.x && p.y >= aMin.y && p.y <= aMax.y; };
    if (inside(aA) || inside(aB))
        return true;
    const ImVec2 d(aB.x - aA.x, aB.y - aA.y);
    float tMin = 0.0f, tMax = 1.0f;
    auto clip = [&](float dirP, float minP, float maxP, float startP) {
        if (std::fabs(dirP) < 1e-6f)
            return !(startP < minP || startP > maxP);
        float t1 = (minP - startP) / dirP;
        float t2 = (maxP - startP) / dirP;
        if (t1 > t2) {
            float tmp = t1;
            t1 = t2;
            t2 = tmp;
        }
        if (t1 > tMin)
            tMin = t1;
        if (t2 < tMax)
            tMax = t2;
        return tMin <= tMax;
    };
    if (!clip(d.x, aMin.x, aMax.x, aA.x))
        return false;
    if (!clip(d.y, aMin.y, aMax.y, aA.y))
        return false;
    return tMin <= 1.0f && tMax >= 0.0f;
}

// Walk a polyline (apPoints[0..aCount-1]) and return true as soon as any
// segment overlaps the AABB. Works for cubic bezier samples, Manhattan
// paths, and host-custom shapes alike — used by box-select on cachedPath.
static bool s_pathIntersectsRect(const ImVec2* apPoints, int aCount, const ImVec2& aMin, const ImVec2& aMax) {
    if (apPoints == nullptr || aCount < 2)
        return false;
    for (int i = 1; i < aCount; ++i) {
        if (s_segmentIntersectsRect(apPoints[i - 1], apPoints[i], aMin, aMax))
            return true;
    }
    return false;
}

// Compute the box-select selection set for the rect (aMn, aMx) and assign
// it into rGraph. Used by both deferred mode (called once on release) and
// live mode (called every frame while the box is active).
//
// Result = base ∪ overlapping (when the multi-select modifier is held) or
//        = overlapping (otherwise — the box replaces the previous set).
// "base" is the pre-box snapshot captured when the box first becomes active.
//
// ExcludeNodes / ExcludeLinks flags skip the corresponding category — useful
// for tools that should only ever pick one kind of object.
static void s_commitBoxSelect(Context& arCtx, GraphState& arGraph, const ImVec2& aMn, const ImVec2& aMx) {
    const bool toggle = s_multiSelectHeld(arGraph);
    const bool excludeNodes = (arGraph.settings.flags & ImNodalGraphFlags_BoxSelectExcludeNodes) != 0;
    const bool excludeLinks = (arGraph.settings.flags & ImNodalGraphFlags_BoxSelectExcludeLinks) != 0;

    std::unordered_set<Id> newNodes = toggle ? arGraph.boxSelectBaseNodes : std::unordered_set<Id>{};
    std::unordered_set<Id> newLinks = toggle ? arGraph.boxSelectBaseLinks : std::unordered_set<Id>{};
    Id touchedNode = 0;
    Id touchedLink = 0;

    if (!excludeNodes) {
        // Nodes : selected as soon as their VISUAL rect intersects the box
        // (AABB-AABB overlap). More forgiving than "center inside the box" :
        // a single corner of the node touching the box is enough.
        for (const auto& kv : arCtx.nodes) {
            if (kv.second.graphId != arGraph.id)
                continue;
            // NotSelectable nodes never enter the box selection.
            if (kv.second.flags & ImNodalNodeFlags_NotSelectable)
                continue;
            const ImRect& r = kv.second.lastScreenRect;
            if (r.Min.x >= r.Max.x)
                continue;
            const bool overlaps = r.Min.x <= aMx.x && r.Max.x >= aMn.x && r.Min.y <= aMx.y && r.Max.y >= aMn.y;
            if (overlaps && newNodes.insert(kv.first).second) {
                touchedNode = kv.first;
            }
        }
    }
    if (!excludeLinks) {
        // Links : selected as soon as the path emitted by the primitives
        // touches the box, even partially. We test chord-vs-rect on each
        // polyline segment — a single grazing intersection is enough.
        // Works for bezier, Manhattan, and any custom shape.
        for (const auto& kv : arCtx.links) {
            if (kv.second.graphId != arGraph.id)
                continue;
            if (!kv.second.pathCached)
                continue;
            if (s_pathIntersectsRect(kv.second.cachedPath.data(), (int)kv.second.cachedPath.size(), aMn, aMx)) {
                if (newLinks.insert(kv.first).second) {
                    touchedLink = kv.first;
                }
            }
        }
    }

    if (newNodes != arGraph.selectedNodes || newLinks != arGraph.selectedLinks) {
        arGraph.selectedNodes = std::move(newNodes);
        arGraph.selectedLinks = std::move(newLinks);
        arGraph.selectionChangedThisFrame = true;
    }
    if (touchedNode != 0)
        arGraph.lastSelectedNode = touchedNode;
    if (touchedLink != 0)
        arGraph.lastSelectedLink = touchedLink;
}

}  // namespace

// -----------------------------
// Graph
// -----------------------------
IMNODAL_API bool BeginGraph(Id aGraphId, const GraphSettings& arSettings) {
    Context& rCtx = s_getCtx();
    // STACK-based scope checks, not pCanvas/pGraph: those pointers keep
    // their "lastTouched" value after End* so post-scope queries still work
    // (GetSelectedNode etc.), but they cannot be used to detect "are we
    // currently inside a scope?". The stacks are the authoritative answer.
    IM_ASSERT(!rCtx.canvasStack.empty() && "BeginGraph must be called inside BeginCanvas/EndCanvas");
    IM_ASSERT(rCtx.graphStack.empty() && "BeginGraph called twice without EndGraph");
    IM_ASSERT(aGraphId != 0 && "Graph id must be non-zero");

    // Scope ID under the graph so multiple graphs in the same canvas don't
    // see each other's widget IDs.
    ImGui::PushID(s_imguiId(aGraphId));

    // Look up (or create) the per-(canvas, graph) state. Each graph stays
    // isolated even when several share the same canvas's view (pan/zoom).
    const GraphKey key{rCtx.canvasStack.back()->id, aGraphId};
    GraphState& rGraph = rCtx.graphs[key];
    rGraph.id = aGraphId;
    rGraph.settings = arSettings;
    rGraph.frameNodeOrder.clear();
    rGraph.frameSlotOrder.clear();
    rGraph.lastSeenFrame = ImGui::GetFrameCount();

    // Push on the graph stack and set the current pointer.
    rCtx.pGraph = &rGraph;
    rCtx.graphStack.push_back(&rGraph);

    // Refresh link "selected" flags from this graph's selection (per-frame).
    for (auto& kv : rCtx.links) {
        if (kv.second.graphId == aGraphId)
            kv.second.selected = (rGraph.selectedLinks.count(kv.first) > 0);
    }

    // Open multi-channel splitter for this graph. Default channel = Content.
    auto* const pDrawList = rCtx.Canvas().drawList;
    rGraph.preBeginChannel = pDrawList->_Splitter._Current;
    pDrawList->ChannelsSplit(GC_Count);
    pDrawList->ChannelsSetCurrent(GC_Content);
    rGraph.splitterActive = true;

    return true;
}

IMNODAL_API void EndGraph() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(!rCtx.graphStack.empty() && "EndGraph without matching BeginGraph");
    IM_ASSERT(rCtx.graphStack.back()->currentNodeId == 0 && "EndGraph while a node is still open");

    GraphState& rGraph = *rCtx.graphStack.back();
    auto* const pDrawList = rCtx.Canvas().drawList;

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
    const bool lmbClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool lmbDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool lmbReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    // Equivalent to rCtx.Canvas().hovered (set at BeginCanvas) — kept as a local for
    // readability of the box-select state machine below.
    const bool lmbOnCanvas = rCtx.Canvas().hovered;
    const ImVec2 mouseLocal = ImGui::GetIO().MousePos;

    // Phase 1: arm a pending bg-click on empty-canvas mouse-down. We store
    // the position in LOCAL-SPACE (for box rendering) AND in SCREEN-SPACE
    // (for the drag threshold — immune to pan/zoom of the canvas that may
    // happen between the click and the check, e.g. if the double-click
    // triggers a fit-to-view : the canvas-space mouse "jumps" while the
    // screen mouse hasn't moved ; we don't want to arm a box-select for
    // that spurious motion).
    if (lmbClicked && lmbOnCanvas && !rGraph.clickConsumedThisFrame && !rCtx.Canvas().isPanning) {
        rCtx.Graph().pendingBgClick = true;
        rCtx.Graph().pendingBgClickPos = mouseLocal;
        rCtx.Graph().pendingBgClickPosScreen = rCtx.Canvas().mousePosBackup;
    }

    // Phase 2: promote pending click to box-select once drag distance crosses
    // the threshold. We compare in SCREEN-SPACE (mousePosBackup vs the screen
    // click position) ; the threshold is in screen pixels, independent of zoom.
    if (rCtx.Graph().pendingBgClick && lmbDown && !rCtx.Graph().boxSelectActive && !(rGraph.settings.flags & ImNodalGraphFlags_NoBoxSelect)) {
        const ImVec2 d = rCtx.Canvas().mousePosBackup - rCtx.Graph().pendingBgClickPosScreen;
        constexpr float kThreshold = 4.0f;  // screen-pixels
        if ((d.x * d.x + d.y * d.y) > (kThreshold * kThreshold)) {
            rCtx.Graph().boxSelectActive = true;
            // The box start must be in CURRENT CANVAS-SPACE : we re-project
            // the screen click position through the current transform (which
            // may have changed if the canvas was zoomed in the meantime —
            // otherwise equivalent to pendingBgClickPos).
            rCtx.Graph().boxSelectStart = (rCtx.Graph().pendingBgClickPosScreen - rCtx.Canvas().viewTransformPos) * rCtx.Canvas().invScale;
        }
    }

    // Phase 3: while box-select is active, draw the rectangle on the overlay
    // channel AND — if BoxSelectLive is set — rebuild the selection live so
    // the user sees what's about to be selected as they drag.
    if (rCtx.Graph().boxSelectActive) {
        const ImVec2 mn(ImMin(rCtx.Graph().boxSelectStart.x, mouseLocal.x), ImMin(rCtx.Graph().boxSelectStart.y, mouseLocal.y));
        const ImVec2 mx(ImMax(rCtx.Graph().boxSelectStart.x, mouseLocal.x), ImMax(rCtx.Graph().boxSelectStart.y, mouseLocal.y));
        s_setChannel(rCtx, GC_Overlay);
        rCtx.Canvas().drawList->AddRectFilled(mn, mx, rCtx.style.Colors[ImNodalCol_BoxSelectFill]);
        rCtx.Canvas().drawList->AddRect(mn, mx, rCtx.style.Colors[ImNodalCol_BoxSelectBorder]);
        s_setChannel(rCtx, GC_Content);

        // Snapshot the pre-box selection ONCE when the box becomes active.
        // The live mode uses this snapshot as the additive base so dragging
        // does not erase pre-existing selections (multi-select modifier
        // semantics). Captured here even for deferred mode in case the
        // host toggles BoxSelectLive on later — cheap and harmless.
        if (!rGraph.boxSelectBaseCaptured) {
            rGraph.boxSelectBaseNodes = rGraph.selectedNodes;
            rGraph.boxSelectBaseLinks = rGraph.selectedLinks;
            rGraph.boxSelectBaseCaptured = true;
        }

        if (rGraph.settings.flags & ImNodalGraphFlags_BoxSelectLive) {
            s_commitBoxSelect(rCtx, rGraph, mn, mx);
        }
    }

    // Phase 4: on release — either commit the box (if it was a drag) or treat
    // as a plain bg-click and clear the selection.
    if (lmbReleased && rCtx.Graph().pendingBgClick) {
        if (rCtx.Graph().boxSelectActive) {
            // Deferred mode does the commit here; live mode already updated
            // the selection every frame and only needs to release the box.
            if (!(rGraph.settings.flags & ImNodalGraphFlags_BoxSelectLive)) {
                const ImVec2 mn(ImMin(rCtx.Graph().boxSelectStart.x, mouseLocal.x), ImMin(rCtx.Graph().boxSelectStart.y, mouseLocal.y));
                const ImVec2 mx(ImMax(rCtx.Graph().boxSelectStart.x, mouseLocal.x), ImMax(rCtx.Graph().boxSelectStart.y, mouseLocal.y));
                s_commitBoxSelect(rCtx, rGraph, mn, mx);
            }
            rCtx.Graph().boxSelectActive = false;
            rGraph.boxSelectBaseNodes.clear();
            rGraph.boxSelectBaseLinks.clear();
            rGraph.boxSelectBaseCaptured = false;
        } else {
            // Plain bg-click → clear selection unless multi-modifier is held.
            if (!s_multiSelectHeld(rGraph)) {
                s_clearSelection(rGraph);
            }
        }
        rCtx.Graph().pendingBgClick = false;
        rCtx.Graph().pendingBgClickPos = ImVec2(0.0f, 0.0f);
    }
    // Reset for next frame
    rGraph.clickConsumedThisFrame = false;

    // BeginSlot/EndSlot is fully capture-only : ImNodal paints NOTHING for
    // the slot here. The host (typically inside its slot draw routine) is
    // responsible for the dot, the hover halo, the connected indicator, etc.
    // Hosts can read IsSlotHovered + GetSlotHitRect / GetSlotScreenPos.
    // The frameSlotOrder loop is gone but the per-frame `connected` flag is
    // still updated by Link() calls earlier in the frame.

    // Merge channels back into the canvas-expected channel order.
    if (rGraph.splitterActive) {
        pDrawList->ChannelsMerge();
        rGraph.splitterActive = false;
    }
    // Splitter must be back to the canvas's expected channel
    IM_ASSERT(pDrawList->_Splitter._Current == rCtx.Canvas().expectedChannel && "EndGraph: splitter not restored to canvas expected channel");

    // Match the PushID at BeginGraph.
    ImGui::PopID();

    // Pop the graph stack. pGraph keeps pointing to the popped state so
    // queries called right after EndGraph (same frame) still see this graph
    // as the "last touched" one — pCanvas behaves the same way at EndCanvas.
    IM_ASSERT(!rCtx.graphStack.empty() && rCtx.graphStack.back() == rCtx.pGraph && "EndGraph: graph stack desync");
    rCtx.graphStack.pop_back();
    rCtx.pGraph = rCtx.graphStack.empty() ? rCtx.pGraph : rCtx.graphStack.back();
    // Note: when graphStack becomes empty, pGraph keeps its last value so
    // post-EndGraph queries (GetSelectedNode, etc.) still work for that
    // frame. NewFrame is what truly resets it.
    // Actually we DO need to clear pGraph when stack is empty BUT keep a
    // "lastTouchedGraph" — let's just leave pGraph as the lastTouched.
}

IMNODAL_API Id GetCurrentGraphId() {
    Context& rCtx = s_getCtx();
    return rCtx.pGraph != nullptr ? rCtx.pGraph->id : (Id)0;
}

// -----------------------------
// Node
// -----------------------------
IMNODAL_API bool BeginNode(Id aNodeId, ImNodalNodeFlags aFlags) {
    Context& rCtx = s_getCtx();
    IM_ASSERT((rCtx.pGraph != nullptr) && "BeginNode must be called inside BeginGraph/EndGraph");
    IM_ASSERT(aNodeId != 0 && "Node id must be non-zero");

    NodeState& rNode = rCtx.nodes[aNodeId];
    rNode.graphId = rCtx.Graph().id;
    rNode.flags = aFlags;
    rNode.customHitbox = ImNodalShape{};  // SetNodeHitbox between Begin/End writes here
    rNode.bodyShape = ImNodalShape{};     // SetNodeBodyShape between Begin/End writes here
    rNode.currentMaxToplevelNatWidth = 0.0f;
    rNode.currentMaxToplevelNatHeight = 0.0f;
    rNode.color = 0;  // SetNodeColor between Begin/End writes here, reset each frame

    // Position is stored on rNode.pos (canvas space). Host pushes initial
    // pos via SetNodePos ; drag updates rNode.pos directly ; GetNodePos
    // reads it back.

    rCtx.Graph().currentNodeId = aNodeId;
    rCtx.Graph().frameNodeOrder.push_back(aNodeId);

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
    IM_ASSERT(rCtx.Graph().currentNodeId != 0 && "EndNode without matching BeginNode");
    IM_ASSERT(rCtx.Graph().layoutStack.empty() && "EndNode while a BeginLayoutHorizontal/BeginLayoutVertical is still open");
    // Defensive : drop any leftover container so the next frame doesn't
    // inherit a corrupt stack even if assertions were compiled out.
    rCtx.Graph().layoutStack.clear();

    NodeState& rNode = rCtx.nodes[rCtx.Graph().currentNodeId];
    GraphState& rGraph = rCtx.Graph();

    ImGui::EndGroup();
    const ImVec2 contentMin = ImGui::GetItemRectMin();
    const ImVec2 contentMax = ImGui::GetItemRectMax();

    // Visual rect = content rect expanded by bodyPadding on each side. This
    // is what the user sees as the node's border, so slot dots (which sit
    // at the content-edge) end up `bodyPadding` away from the visible border.
    const float pad = rCtx.style.NodeBodyPadding;
    const ImVec2 nodeMin(contentMin.x - pad, contentMin.y - pad);
    const ImVec2 nodeMax(contentMax.x + pad, contentMax.y + pad);
    rNode.size = nodeMax - nodeMin;
    // Snapshot the running max for next frame's "fill parent" target.
    rNode.lastFrameMaxToplevelNatWidth = rNode.currentMaxToplevelNatWidth;
    rNode.lastFrameMaxToplevelNatHeight = rNode.currentMaxToplevelNatHeight;

    // Promote staging body shape (set by SetNodeBodyShape) + staging hitbox
    // (set by SetNodeHitbox) to the node's persistent state, then clear
    // staging so the next node starts fresh. Reset BEFORE the hit-test reads
    // the customHitbox so a missing call falls back to the rectangular default.
    // Auto-hitbox rule : when the host set a body shape but did NOT call
    // SetNodeHitbox, the body shape is used as the hit area too — the common
    // case is "shape == hit area" so the host shouldn't have to repeat itself.
    // An explicit SetNodeHitbox still wins (host wants a hit area different
    // from the visible body — e.g. larger for easier clicking).
    if (rCtx.Canvas().stagingNodeBodyShape.type != ImNodalHitShape_None) {
        rNode.bodyShape = rCtx.Canvas().stagingNodeBodyShape;
    }
    if (rCtx.Canvas().stagingNodeHitbox.type != ImNodalHitShape_None) {
        rNode.customHitbox = rCtx.Canvas().stagingNodeHitbox;
    } else if (rCtx.Canvas().stagingNodeBodyShape.type != ImNodalHitShape_None) {
        rNode.customHitbox = rCtx.Canvas().stagingNodeBodyShape;
    }
    rCtx.Canvas().stagingNodeHitbox = ImNodalShape{};
    rCtx.Canvas().stagingNodeBodyShape = ImNodalShape{};

    // Hover test : the rectangular fallback covers the legacy "node = AABB"
    // behavior. A custom hitbox (SetNodeHitbox) overrides the shape — used
    // for circular reroute dots, diamond decision nodes, ...
    // Canvas-hovered gate : a node never registers as hovered when the mouse
    // sits on a side panel, another window or another canvas — even if its
    // screen rect happens to cover the cursor position there.
    {
        const ImRect fallback(nodeMin, nodeMax);
        const ImVec2 mp = ImGui::GetIO().MousePos;
        rNode.hovered = s_hitTestShape(rNode.customHitbox, mp, fallback) && rCtx.Canvas().hovered;
    }
    if (rNode.hovered) {
        rCtx.Graph().currentHoveredNode = rCtx.Graph().currentNodeId;
    }

    // ---- Selection + drag (left-click-hold on the node rect) ----
    const bool lmbClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool lmbDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool lmbReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    const bool rmbClicked = ImGui::IsMouseClicked(rCtx.Canvas().settings.contextMenuButton);
    // "topmost" = mouse on node AND no user widget is above the cursor.
    const bool hoveredTopMost = rNode.hovered && !ImGui::IsAnyItemHovered();

    const bool notSelectable = (rNode.flags & ImNodalNodeFlags_NotSelectable) != 0;
    const bool notMovable = (rNode.flags & ImNodalNodeFlags_NotMovable) != 0;
    if (hoveredTopMost && lmbClicked && !notSelectable) {
        const bool toggle = s_multiSelectHeld(rGraph);
        s_selectNode(rGraph, rCtx.Graph().currentNodeId, toggle);
        rGraph.clickConsumedThisFrame = true;
        // Don't start a drag when toggling — the user is building a selection,
        // not moving the node.
        if (!notMovable && !toggle) {
            rCtx.Graph().draggingNodeId = rCtx.Graph().currentNodeId;
            rCtx.Graph().dragStartNodePos = rNode.pos;
            rCtx.Graph().dragStartMouseCanvas = ImGui::GetIO().MousePos;  // canvas space (we're in local space)
        }
    }
    if (rCtx.Graph().draggingNodeId == rCtx.Graph().currentNodeId) {
        if (lmbDown) {
            const ImVec2 delta = ImGui::GetIO().MousePos - rCtx.Graph().dragStartMouseCanvas;
            rNode.pos = rCtx.Graph().dragStartNodePos + delta;
            rNode.dragging = true;
        }
        if (lmbReleased) {
            rCtx.Graph().draggingNodeId = 0;
            rNode.dragging = false;
        }
    } else {
        rNode.dragging = false;
    }
    rNode.selected = (rGraph.selectedNodes.count(rCtx.Graph().currentNodeId) > 0);

    // Right-click on the node rect (and not on a widget above) → context menu request.
    if (hoveredTopMost && rmbClicked) {
        rNode.ctxMenuRequested = true;
        rCtx.Graph().ctxMenuNodeId = rCtx.Graph().currentNodeId;
    }

    auto* const pDrawList = rCtx.Canvas().drawList;
    const Style& s = rCtx.style;
    const float rounding = s.NodeRounding;
    const bool noBody = (rNode.flags & ImNodalNodeFlags_NoBody) != 0;
    // Body fill on BgFill (deepest node channel) + border on BgBorder. The
    // host can layer its own tint (header band, etc.) on UserBg ABOVE the
    // fill and BELOW the border. When SetNodeBodyShape was called, the
    // fill/border follow the shape (circle / convex polygon) instead of the
    // default rect ; NodeRounding is ignored in that case.
    if (!noBody) {
        const ImU32 fillCol = s.Colors[ImNodalCol_NodeBody];
        const ImU32 borderCol = rNode.selected ? s.Colors[ImNodalCol_NodeBorderSelected] : s.Colors[ImNodalCol_NodeBorder];
        const float borderThk = s.NodeBorderThickness;
        switch (rNode.bodyShape.type) {
            case ImNodalHitShape_Circle: {
                s_setChannel(rCtx, GC_BgFill);
                pDrawList->AddCircleFilled(rNode.bodyShape.center, rNode.bodyShape.radius, fillCol);
                s_setChannel(rCtx, GC_BgBorder);
                pDrawList->AddCircle(rNode.bodyShape.center, rNode.bodyShape.radius, borderCol, 0, borderThk);
                break;
            }
            case ImNodalHitShape_ConvexPolygon: {
                s_setChannel(rCtx, GC_BgFill);
                pDrawList->AddConvexPolyFilled(rNode.bodyShape.polygonPoints, rNode.bodyShape.polygonCount, fillCol);
                s_setChannel(rCtx, GC_BgBorder);
                pDrawList->AddPolyline(rNode.bodyShape.polygonPoints, rNode.bodyShape.polygonCount,
                                       borderCol, borderThk, ImDrawFlags_Closed);
                break;
            }
            case ImNodalHitShape_Rect:
            case ImNodalHitShape_None:
            default: {
                s_setChannel(rCtx, GC_BgFill);
                pDrawList->AddRectFilled(nodeMin, nodeMax, fillCol, rounding);
                s_setChannel(rCtx, GC_BgBorder);
                pDrawList->AddRect(nodeMin, nodeMax, borderCol, rounding, borderThk);
                break;
            }
        }
    }
    if ((rNode.flags & ImNodalNodeFlags_HoverHandle) && rNode.hovered) {
        s_setChannel(rCtx, GC_BgBorder);
        const float h = s.NodeHoverHandleHeight;
        const ImVec2 bMin(nodeMin.x + 2.0f, nodeMin.y + 1.0f);
        const ImVec2 bMax(nodeMax.x - 2.0f, nodeMin.y + 1.0f + h);
        pDrawList->AddRectFilled(bMin, bMax, s.Colors[ImNodalCol_NodeHoverHandle], h * 0.5f);
    }

    // Restore content channel for next widgets
    s_setChannel(rCtx, GC_Content);

    // Cache the visual rect (local-space) for GetNodeRect / per-node drawlists.
    rNode.lastScreenRect = ImRect(nodeMin, nodeMax);

    ImGui::PopID();

    rCtx.Graph().currentNodeId = 0;
}

IMNODAL_API void SetNodeColor(ImU32 aColor) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Graph().currentNodeId != 0 && "SetNodeColor must be called between BeginNode/EndNode");
    auto it = rCtx.nodes.find(rCtx.Graph().currentNodeId);
    if (it == rCtx.nodes.end())
        return;
    it->second.color = aColor;
}

IMNODAL_API void SetNodePos(ImVec2 aPos) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Graph().currentNodeId != 0 && "SetNodePos must be called between BeginNode/EndNode (use SetNextNodePos with an id outside the scope)");
    auto it = rCtx.nodes.find(rCtx.Graph().currentNodeId);
    if (it == rCtx.nodes.end())
        return;
    it->second.pos = aPos;
}

IMNODAL_API void SetNextNodePos(Id aNodeId, ImVec2 aPos) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(aNodeId != 0 && "Node id must be non-zero");
    rCtx.nodes[aNodeId].pos = aPos;
}

IMNODAL_API ImVec2 GetNodePos(Id aNodeId) {
    Context& rCtx = s_getCtx();
    auto it = rCtx.nodes.find(aNodeId);
    return (it != rCtx.nodes.end()) ? it->second.pos : ImVec2(0.0f, 0.0f);
}

// =====================================================================
// Layout primitives — BeginLayoutHorizontal/Vertical/Group + LayoutSpring
// =====================================================================
namespace {

// Resolve container target size from the user-supplied aSize :
//   > 0 : use as-is.
//   == 0 : natural (= sum of non-Spring children measured at last frame).
//          Spring inside is a no-op since gap == 0.
//   < 0 : fill parent. Parent target = the enclosing layout container if any,
//         else the node body width/height (= node.size - 2 * NodeBodyPadding).
inline ImVec2 s_resolveLayoutTarget(Context& arCtx, const ImVec2& aSize) {
    ImVec2 target = aSize;
    if (aSize.x >= 0.0f && aSize.y >= 0.0f) {
        return target;
    }
    // Compute parent target along axes that need filling.
    ImVec2 parentTarget(0.0f, 0.0f);
    if (!arCtx.Graph().layoutStack.empty()) {
        parentTarget = arCtx.Graph().layoutStack.back().targetSize;
    } else if (arCtx.Graph().currentNodeId != 0) {
        // Top-level container of a node : reference is the max natural size
        // of the OTHER top-level containers (header, footer…) measured at the
        // previous frame. NOT node.size — using node.size would be circular
        // (node.size depends on body width which depends on Spring fill which
        // depends on target which depends on node.size), and that loop bakes
        // the current padding into Spring fill so changing pad never resets
        // the inter-slot gap.
        auto it = arCtx.nodes.find(arCtx.Graph().currentNodeId);
        if (it != arCtx.nodes.end()) {
            parentTarget.x = it->second.lastFrameMaxToplevelNatWidth;
            parentTarget.y = it->second.lastFrameMaxToplevelNatHeight;
        }
    }
    if (aSize.x < 0.0f) target.x = parentTarget.x;
    if (aSize.y < 0.0f) target.y = parentTarget.y;
    return target;
}

// Pre-position the cursor on the parent's row when the parent is horizontal
// and we're about to start a non-first child. Increments the parent's child
// counter so subsequent children also get a SameLine. No-op when there is no
// parent or when the parent is vertical.
inline void s_emitChildSameLineIfH(Context& arCtx) {
    if (arCtx.Graph().layoutStack.empty()) {
        return;
    }
    LayoutContainer& parent = arCtx.Graph().layoutStack.back();
    if (parent.isHorizontal && parent.childCount > 0) {
        ImGui::SameLine(0.0f, 0.0f);
    }
    parent.childCount++;
}

inline bool s_beginLayout(Context& arCtx, const char* aId, const ImVec2& aSize, bool aHorizontal) {
    IM_ASSERT(arCtx.Graph().currentNodeId != 0 && "BeginLayoutHorizontal/Vertical must be called inside BeginNode/EndNode");
    IM_ASSERT(aId != nullptr && "BeginLayoutHorizontal/Vertical: id must be non-null");

    s_emitChildSameLineIfH(arCtx);

    ImGuiWindow* const pWindow = ImGui::GetCurrentWindow();
    const ImGuiID id = pWindow->GetID(aId);

    LayoutContainer c;
    c.id = id;
    c.isHorizontal = aHorizontal;
    c.startCursorScreen = ImGui::GetCursorScreenPos();
    c.targetSize = s_resolveLayoutTarget(arCtx, aSize);
    c.consumedAlongAxis = 0.0f;
    c.springsTotalWeight = 0.0f;
    c.childCount = 0;
    arCtx.Graph().layoutStack.push_back(c);

    ImGui::PushID(aId);
    ImGui::BeginGroup();
    return true;
}

inline void s_endLayout(Context& arCtx, bool aHorizontal) {
    IM_ASSERT(!arCtx.Graph().layoutStack.empty() && "EndLayoutHorizontal/Vertical without matching BeginLayoutHorizontal/Vertical");
    LayoutContainer c = arCtx.Graph().layoutStack.back();
    IM_ASSERT(c.isHorizontal == aHorizontal && "EndLayoutHorizontal while a BeginLayoutVertical is open (or vice versa)");
    arCtx.Graph().layoutStack.pop_back();

    ImGui::EndGroup();
    ImGui::PopID();

    // total = naturals + spring fills emitted along the main axis.
    // natural = total - consumed (= sum of non-spring children sizes).
    LayoutSlot& s = arCtx.Graph().layoutSlots[c.id];
    const ImVec2 total = ImGui::GetItemRectSize();
    float natW, natH;
    if (aHorizontal) {
        natW = ImMax(0.0f, total.x - c.consumedAlongAxis);
        natH = total.y;
    } else {
        natW = total.x;
        natH = ImMax(0.0f, total.y - c.consumedAlongAxis);
    }
    s.lastNatWidth = natW;
    s.lastNatHeight = natH;
    // Snapshot the total Spring weight emitted this frame so next frame's
    // Springs each take their proportional share of the gap.
    s.lastSpringsTotalWeight = c.springsTotalWeight;

    // Top-level container of the current node : feed our natural size to the
    // node's running max, so sibling top-level containers can use it as their
    // "fill parent" target on the NEXT frame. Decouples target from node.size
    // (which is itself derived from this container's width with Spring fill,
    // creating an auto-referential loop that bakes the current padding into
    // the next frame's Spring fill).
    if (arCtx.Graph().layoutStack.empty() && arCtx.Graph().currentNodeId != 0) {
        NodeState& rNode = arCtx.nodes[arCtx.Graph().currentNodeId];
        rNode.currentMaxToplevelNatWidth = ImMax(rNode.currentMaxToplevelNatWidth, natW);
        rNode.currentMaxToplevelNatHeight = ImMax(rNode.currentMaxToplevelNatHeight, natH);
    }
}

}  // namespace

IMNODAL_API bool BeginLayoutHorizontal(const char* aId, const ImVec2& aSize) {
    return s_beginLayout(s_getCtx(), aId, aSize, /*horizontal=*/true);
}
IMNODAL_API void EndLayoutHorizontal() {
    s_endLayout(s_getCtx(), /*horizontal=*/true);
}
IMNODAL_API bool BeginLayoutVertical(const char* aId, const ImVec2& aSize) {
    return s_beginLayout(s_getCtx(), aId, aSize, /*horizontal=*/false);
}
IMNODAL_API void EndLayoutVertical() {
    s_endLayout(s_getCtx(), /*horizontal=*/false);
}

IMNODAL_API bool BeginLayoutGroup() {
    Context& rCtx = s_getCtx();
    s_emitChildSameLineIfH(rCtx);  // no-op si layoutStack vide ou si parent vertical
    ImGui::BeginGroup();
    return true;
}
IMNODAL_API void EndLayoutGroup() {
    ImGui::EndGroup();
}

IMNODAL_API void LayoutSpring(float aWeight) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(!rCtx.Graph().layoutStack.empty() && "LayoutSpring outside of BeginLayoutHorizontal/BeginLayoutVertical scope");
    if (aWeight <= 0.0f) {
        return;
    }
    s_emitChildSameLineIfH(rCtx);

    LayoutContainer& c = rCtx.Graph().layoutStack.back();
    LayoutSlot& s = rCtx.Graph().layoutSlots[c.id];

    // Multi-Spring distribution : each Spring takes (gap * weight / totalWeight),
    // where totalWeight is the sum of weights measured at the PREVIOUS frame.
    // This breaks the divergence loop : with 2 Springs in a header (centering
    // pattern), each takes half the gap so the total never exceeds target.
    // First frame on this container : lastSpringsTotalWeight is 0 → fill 0
    // (we don't know the total yet ; the container stays at natural width
    // for the first frame, then Springs converge from frame 2).
    const float target = c.isHorizontal ? c.targetSize.x : c.targetSize.y;
    const float lastNat = c.isHorizontal ? s.lastNatWidth : s.lastNatHeight;
    const float gap = ImMax(0.0f, target - lastNat);
    const float fill = (s.lastSpringsTotalWeight > 0.0f) ? (gap * aWeight / s.lastSpringsTotalWeight) : 0.0f;

    c.springsTotalWeight += aWeight;

    // Always emit a Dummy (even with fill == 0) so CursorPosPrevLine points
    // at the right spot for the next sibling's implicit SameLine. Without
    // this, a 0-fill Spring still increments childCount and the next BeginLayoutVertical
    // SameLine(0,0)s back to BEFORE the parent BeginLayoutHorizontal (= the previous line),
    // which dumps the next sibling on top of the previous block.
    if (c.isHorizontal) {
        ImGui::Dummy(ImVec2(fill, 0.0f));
        // Stay on the row so the next sibling lands at the right of us even
        // if it doesn't go through s_emitChildSameLineIfH.
        ImGui::SameLine(0.0f, 0.0f);
    } else {
        ImGui::Dummy(ImVec2(0.0f, fill));
    }
    c.consumedAlongAxis += fill;
}

// -----------------------------
// Slot primitive
// -----------------------------
IMNODAL_API bool BeginSlot(Id aSlotId, ImNodalSlotFlags aFlags) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(aSlotId != 0 && "Slot id must be non-zero");
    IM_ASSERT(rCtx.Graph().currentSlotId == 0 && "Nested BeginSlot is not supported");
    SlotState& rSlot = rCtx.slots[aSlotId];
    rSlot.parentNode = rCtx.Graph().currentNodeId;
    rSlot.graphId = rCtx.Graph().id;
    rSlot.flags = aFlags;
    // SetSlotHitbox / SetSlotBodyShape between Begin/End write here. Reset
    // before the early hover test so a missing call falls back to the
    // last-frame rect (hitbox) or to "no paint" (body shape).
    rSlot.customHitbox = ImNodalShape{};
    rSlot.bodyShape = ImNodalShape{};

    // EARLY hover hit-test against the previous frame's hit shape. Without this,
    // rSlot.hovered would stay false until EndSlot — but the host typically
    // queries IsSlotHovered() / double-click handlers BETWEEN BeginSlot and
    // EndSlot, so they'd always read false. Using last frame's rect introduces
    // a 1-frame lag that's invisible for stable layouts (which is the common
    // case). EndSlot will refine the value with this frame's actual rect.
    // Canvas-hovered gate : when inside a canvas, only hit-test if the mouse
    // is actually on this canvas. Outside a canvas (e.g. slot anatomy demo
    // emitted directly in a window), no gating — geometry alone decides.
    {
        const ImRect& r = rSlot.lastHitRect;
        const bool gateOK = (!rCtx.Canvas().active) || rCtx.Canvas().hovered;
        if (gateOK && r.Min.x < r.Max.x && r.Min.y < r.Max.y) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            rSlot.hovered = (mp.x >= r.Min.x && mp.x <= r.Max.x && mp.y >= r.Min.y && mp.y <= r.Max.y);
        } else {
            rSlot.hovered = false;
        }
    }

    rCtx.Graph().currentSlotId = aSlotId;

    ImGui::PushID(s_imguiId(aSlotId));
    ImGui::BeginGroup();
    // Snapshot cursor right after BeginGroup. EndSlot uses this to detect a
    // host that emitted nothing inside the slot (cursor unchanged) and inserts
    // a (2r, 2r) Dummy so the slot still has a visible dot + hit area.
    rCtx.Graph().currentSlotBeginCursor = ImGui::GetCursorScreenPos();
    return true;
}

IMNODAL_API void EndSlot() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Graph().currentSlotId != 0 && "EndSlot without matching BeginSlot");

    SlotState& rSlot = rCtx.slots[rCtx.Graph().currentSlotId];

    // Empty-slot fallback : if the host emitted nothing between BeginSlot
    // and EndSlot, the cursor is still at the BeginGroup position. Insert a
    // Dummy of `Style.SlotMinSize` so the slot has a hit area the host can
    // hover and link-drag from. The host then paints its own dot/icon at
    // GetSlotScreenPos(slotId). Skip when SlotFlags_NoEmptyDummy is set —
    // the host wants the slot rect to be exactly the host content (or
    // empty if there's no content at all).
    if (!(rSlot.flags & ImNodalSlotFlags_NoEmptyDummy)) {
        const ImVec2 cur = ImGui::GetCursorScreenPos();
        if (cur.x == rCtx.Graph().currentSlotBeginCursor.x && cur.y == rCtx.Graph().currentSlotBeginCursor.y) {
            ImGui::Dummy(rCtx.style.SlotMinSize);
        }
    }

    ImGui::EndGroup();
    const ImVec2 gMin = ImGui::GetItemRectMin();
    const ImVec2 gMax = ImGui::GetItemRectMax();

    // Pivot rect — same model as thedmd/imgui-node-editor.
    //   pivot.Min = group.Min + group.Size * slotAlignment
    //   pivot.Max = pivot.Min + slotSize
    // Defaults : slotAlignment = (0.5, 0.5) (group center), slotSize = (0,0)
    // (point pivot). Hosts override per-slot via SlotAlignment / SlotSize.
    const ImVec2 gSize = gMax - gMin;
    const ImVec2 pivotMin = gMin + gSize * rCtx.Canvas().slotAlignment;
    const ImVec2 pivotMax = pivotMin + rCtx.Canvas().slotSize;
    rSlot.screenPos = (pivotMin + pivotMax) * 0.5f + rCtx.Canvas().slotPivotOffset;
    // Tangent derived from slotAlignment :
    //   alignment.x near 0   -> tangent (-1, 0)   (left edge -> link goes left)
    //   alignment.x near 1   -> tangent ( 1, 0)   (right edge -> link goes right)
    //   alignment.y near 0   -> tangent ( 0, -1)  (top edge -> link goes up)
    //   alignment.y near 1   -> tangent ( 0,  1)  (bottom edge -> link goes down)
    //   centered             -> tangent ( 0,  0)  (resolved dynamically by Link)
    {
        const float ax = rCtx.Canvas().slotAlignment.x;
        const float ay = rCtx.Canvas().slotAlignment.y;
        const float dx = ax - 0.5f;  // negative -> left, positive -> right
        const float dy = ay - 0.5f;  // negative -> up,   positive -> down
        if (std::fabs(dx) < 0.25f && std::fabs(dy) < 0.25f) {
            rSlot.tangent = ImVec2(0.0f, 0.0f);  // centered -> dynamic
        } else if (std::fabs(dx) >= std::fabs(dy)) {
            rSlot.tangent = ImVec2(dx >= 0.0f ? 1.0f : -1.0f, 0.0f);
        } else {
            rSlot.tangent = ImVec2(0.0f, dy >= 0.0f ? 1.0f : -1.0f);
        }
    }
    // Absolute overrides : SetSlotAnchor / SetSlotTangent bypass the
    // group-rect-relative math entirely. When set, the host has placed the
    // link endpoint exactly where it wants it — typically on a precise
    // point of a custom polygonal slot.
    if (rCtx.Canvas().slotAnchorSet) {
        rSlot.screenPos = rCtx.Canvas().slotAnchorPos;
    }
    if (rCtx.Canvas().slotTangentSet) {
        rSlot.tangent = rCtx.Canvas().slotTangentDir;
    }

    // Reset pivot overrides for the next slot — back to thedmd defaults.
    rCtx.Canvas().slotAlignment = ImVec2(0.5f, 0.5f);
    rCtx.Canvas().slotSize = ImVec2(0.0f, 0.0f);
    rCtx.Canvas().slotPivotOffset = ImVec2(0.0f, 0.0f);
    rCtx.Canvas().slotAnchorSet = false;
    rCtx.Canvas().slotAnchorPos = ImVec2(0.0f, 0.0f);
    rCtx.Canvas().slotTangentSet = false;
    rCtx.Canvas().slotTangentDir = ImVec2(0.0f, 0.0f);

    // Promote staging body shape (SetSlotBodyShape) + staging hitbox
    // (SetSlotHitbox) to the slot's persistent state, then clear staging.
    // Auto-hitbox rule : SetSlotBodyShape also sets the hit area to the
    // same shape when SetSlotHitbox was NOT called — common case is "hit
    // area follows the visual". Explicit SetSlotHitbox still wins.
    if (rCtx.Canvas().stagingSlotBodyShape.type != ImNodalHitShape_None) {
        rSlot.bodyShape = rCtx.Canvas().stagingSlotBodyShape;
    }
    if (rCtx.Canvas().stagingSlotHitbox.type != ImNodalHitShape_None) {
        rSlot.customHitbox = rCtx.Canvas().stagingSlotHitbox;
    } else if (rCtx.Canvas().stagingSlotBodyShape.type != ImNodalHitShape_None) {
        rSlot.customHitbox = rCtx.Canvas().stagingSlotBodyShape;
    }
    rCtx.Canvas().stagingSlotHitbox = ImNodalShape{};
    rCtx.Canvas().stagingSlotBodyShape = ImNodalShape{};

    // Hit area : the group rect by default. SetSlotHitbox lets the host
    // override the shape (circle for reroute dots, polygon for diamond
    // corners, rect for arbitrary screen-space areas). The AABB englobing
    // the shape is what ImGui::ItemAdd / culling consume; the actual hover
    // / click test goes through s_hitTestShape so the shape itself decides.
    const ImRect groupRect(gMin, gMax);
    const ImRect hitBB = s_hitTestAABB(rSlot.customHitbox, groupRect);
    // Cache for next frame's BeginSlot early hover test + EndGraph hover frame.
    rSlot.lastHitRect = hitBB;

    // Treat the slot as a single button-like widget regardless of where it
    // lives (inside a node section, inline in a window, ...). ButtonBehavior
    // handles hover/click/ActiveId; the host window won't move because the
    // slot owns ActiveId once held.
    // ID derived from the slot's logical Id (not from the ImGui ID stack):
    // a window->GetID("##fixed") collides across slots when the host code
    // between Begin/EndSlot perturbs the current window context.
    const ImGuiID hitId = ImHashStr("##imnodal_slot_hit", 0, s_imguiId(rCtx.Graph().currentSlotId));
    ImGui::KeepAliveID(hitId);
    bool btnHovered = false, btnHeld = false;
    if (ImGui::ItemAdd(hitBB, hitId)) {
        ImGui::ButtonBehavior(hitBB, hitId, &btnHovered, &btnHeld, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    }
    // Raw geometric hover in parallel: while dragging from slot A,
    // ButtonBehavior on slot B reports hovered=false (ActiveId gating).
    // The raw test bypasses that so the target dot still highlights and
    // currentHoveredSlot tracks correctly for link-target detection.
    // Canvas-hovered gate : same rule as BeginSlot above — only gate when
    // we're actually inside a canvas scope (anatomy outside canvas keeps
    // its geometric-only behavior).
    const bool gateOK = (!rCtx.Canvas().active) || rCtx.Canvas().hovered;
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const bool mouseOnSlot = gateOK && s_hitTestShape(rSlot.customHitbox, mp, groupRect);
    // ButtonBehavior reports hovered using the AABB; refine with the actual
    // shape so a circular slot doesn't grab clicks in the rect's empty
    // corners. (Cheap : the shape was already evaluated above.)
    const bool buttonShapeHover = btnHovered && s_hitTestShape(rSlot.customHitbox, mp, groupRect);
    rSlot.hovered = buttonShapeHover || mouseOnSlot;
    if (mouseOnSlot) {
        rCtx.Graph().currentHoveredSlot = rCtx.Graph().currentSlotId;
    }

    // Start a connection drag on click.
    const bool noConnStart = (rSlot.flags & ImNodalSlotFlags_NoConnectionStart) != 0;
    if (!noConnStart && buttonShapeHover && btnHeld && rCtx.Graph().draggingFromSlot == 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        rCtx.Graph().draggingFromSlot = rCtx.Graph().currentSlotId;
        if ((rCtx.pGraph != nullptr)) {
            rCtx.Graph().clickConsumedThisFrame = true;
        }
    }

    // Right-click on the slot → context menu request.
    const bool noCtxMenu = (rSlot.flags & ImNodalSlotFlags_NoContextMenu) != 0;
    if (!noCtxMenu && mouseOnSlot && ImGui::IsMouseClicked(rCtx.Canvas().settings.contextMenuButton)) {
        rSlot.ctxMenuRequested = true;
        rCtx.Graph().ctxMenuSlotId = rCtx.Graph().currentSlotId;
    }

    // Register the slot so EndGraph can paint the interaction hover frame
    // and let Link() flip the connected flag. ImNodal does NOT draw the dot
    // here — the host paints whatever mark it wants at GetSlotScreenPos,
    // OR it calls SetSlotBodyShape and lets the lib paint the shape with
    // theme colors (block below).
    if ((rCtx.pGraph != nullptr)) {
        rCtx.Graph().frameSlotOrder.push_back(rCtx.Graph().currentSlotId);
    }

    // Paint the body shape (set by SetSlotBodyShape) with theme colors. The
    // drawList is the canvas drawList when inside a canvas, the current
    // window drawList for standalone slots. Drawn on the current channel so
    // host widgets emitted between Begin/EndSlot end up UNDER the body —
    // the typical "shape == slot visual" pattern has no inner content.
    if (rSlot.bodyShape.type != ImNodalHitShape_None) {
        ImDrawList* const pDrawList = rCtx.Canvas().drawList != nullptr ? rCtx.Canvas().drawList : ImGui::GetWindowDrawList();
        // Fill priority : hovered (this-frame, accurate) > connected (uses
        // connectedLastFrame because Link() runs after EndSlot) > body.
        const ImU32 fillCol =
            rSlot.hovered ? rCtx.style.Colors[ImNodalCol_SlotHovered]
            : rSlot.connectedLastFrame ? rCtx.style.Colors[ImNodalCol_SlotConnected]
            : rCtx.style.Colors[ImNodalCol_SlotBody];
        const ImU32 borderCol = rCtx.style.Colors[ImNodalCol_SlotBorder];
        const float borderThk = rCtx.style.SlotBorderThickness;
        switch (rSlot.bodyShape.type) {
            case ImNodalHitShape_Circle: {
                pDrawList->AddCircleFilled(rSlot.bodyShape.center, rSlot.bodyShape.radius, fillCol);
                pDrawList->AddCircle(rSlot.bodyShape.center, rSlot.bodyShape.radius, borderCol, 0, borderThk);
                break;
            }
            case ImNodalHitShape_ConvexPolygon: {
                pDrawList->AddConvexPolyFilled(rSlot.bodyShape.polygonPoints, rSlot.bodyShape.polygonCount, fillCol);
                pDrawList->AddPolyline(rSlot.bodyShape.polygonPoints, rSlot.bodyShape.polygonCount, borderCol, borderThk, ImDrawFlags_Closed);
                break;
            }
            case ImNodalHitShape_Rect: {
                pDrawList->AddRectFilled(rSlot.bodyShape.rect.Min, rSlot.bodyShape.rect.Max, fillCol);
                pDrawList->AddRect(rSlot.bodyShape.rect.Min, rSlot.bodyShape.rect.Max, borderCol, 0.0f, borderThk);
                break;
            }
            case ImNodalHitShape_None:
            default:
                break;
        }
    }

    ImGui::PopID();
    rCtx.Graph().currentSlotId = 0;
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
IMNODAL_API ImRect GetSlotHitRect(Id aSlotId) {
    Context& rCtx = s_getCtx();
    auto it = rCtx.slots.find(aSlotId);
    return (it != rCtx.slots.end()) ? it->second.lastHitRect : ImRect();
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
// No-arg overloads — read the current slot from the active BeginSlot scope.
// Ergonomic equivalent to IsSlotHovered(currentSlotId) inside a custom slot
// drawing block. Assert if no slot is currently open.
IMNODAL_API bool IsSlotHovered() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Graph().currentSlotId != 0 && "IsSlotHovered() (no-arg) must be called between BeginSlot/EndSlot — use IsSlotHovered(slotId) outside that scope");
    return IsSlotHovered(rCtx.Graph().currentSlotId);
}
IMNODAL_API bool IsSlotConnected() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Graph().currentSlotId != 0 && "IsSlotConnected() (no-arg) must be called between BeginSlot/EndSlot — use IsSlotConnected(slotId) outside that scope");
    // Combine connected (set by Link() this frame) || connectedLastFrame
    // (snapshot from previous frame's Link() pass) so the result is correct
    // whether the host queries BEFORE or AFTER the Link() calls.
    auto it = rCtx.slots.find(rCtx.Graph().currentSlotId);
    return (it != rCtx.slots.end()) && (it->second.connected || it->second.connectedLastFrame);
}

IMNODAL_API bool IsNodeHovered(Id* apoNodeId) {
    Context& rCtx = s_getCtx();
    IM_ASSERT((rCtx.pGraph != nullptr) && "IsNodeHovered must be called inside BeginGraph/EndGraph");
    GraphState& rGraph = rCtx.Graph();
    // Walk draw order reverse so top-most wins
    for (int i = (int)rGraph.frameNodeOrder.size() - 1; i >= 0; --i) {
        Id nid = rGraph.frameNodeOrder[i];
        auto it = rCtx.nodes.find(nid);
        if (it != rCtx.nodes.end() && it->second.hovered) {
            if (apoNodeId)
                *apoNodeId = nid;
            return true;
        }
    }
    return false;
}
IMNODAL_API bool IsNodeSelected(Id aNodeId) {
    Context& rCtx = s_getCtx();
    IM_ASSERT((rCtx.pGraph != nullptr) && "IsNodeSelected must be called inside BeginGraph/EndGraph");
    return rCtx.Graph().selectedNodes.count(aNodeId) > 0;
}
IMNODAL_API Id GetSelectedNode() {
    Context& rCtx = s_getCtx();
    IM_ASSERT((rCtx.pGraph != nullptr) && "GetSelectedNode must be called inside BeginGraph/EndGraph");
    GraphState& g = rCtx.Graph();
    if (g.lastSelectedNode != 0 && g.selectedNodes.count(g.lastSelectedNode) > 0) {
        return g.lastSelectedNode;
    }
    return g.selectedNodes.empty() ? (Id)0 : *g.selectedNodes.begin();
}
IMNODAL_API void SetSelectedNode(Id aNodeId) {
    Context& rCtx = s_getCtx();
    IM_ASSERT((rCtx.pGraph != nullptr) && "SetSelectedNode must be called inside BeginGraph/EndGraph");
    GraphState& g = rCtx.Graph();
    if (aNodeId == 0) {
        s_clearSelection(g);
    } else {
        s_selectNode(g, aNodeId, false);
    }
}
IMNODAL_API bool IsNodeDragging(Id aNodeId) {
    Context& rCtx = s_getCtx();
    return rCtx.Graph().draggingNodeId == aNodeId;
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
    IM_ASSERT((rCtx.pGraph != nullptr) && "IsLinkSelected must be called inside BeginGraph/EndGraph");
    return rCtx.Graph().selectedLinks.count(aLinkId) > 0;
}
IMNODAL_API Id GetSelectedLink() {
    Context& rCtx = s_getCtx();
    if ((rCtx.pGraph != nullptr)) {
        GraphState& g = rCtx.Graph();
        if (g.lastSelectedLink != 0 && g.selectedLinks.count(g.lastSelectedLink) > 0) {
            return g.lastSelectedLink;
        }
        return g.selectedLinks.empty() ? (Id)0 : *g.selectedLinks.begin();
    }
    return rCtx.Canvas().standaloneSelectedLink;
}
IMNODAL_API void SetSelectedLink(Id aLinkId) {
    Context& rCtx = s_getCtx();
    if ((rCtx.pGraph != nullptr)) {
        GraphState& g = rCtx.Graph();
        if (aLinkId == 0) {
            s_clearSelection(g);
        } else {
            s_selectLink(g, aLinkId, false);
        }
    } else {
        rCtx.Canvas().standaloneSelectedLink = aLinkId;
    }
}

// =====================================================================
// Links (M2)
// =====================================================================

namespace {

// Squared distance from `aPt` to the line segment (`aA`, `aB`). Returns 0
// when the projection lands inside the segment and the point is exactly on
// the line. Used by LinkLineSegment hit-testing — the OBB of width
// (threshold*2) along the segment is exactly { d² ≤ threshold² }.
inline float s_pointToSegmentDistanceSq(const ImVec2& aPt, const ImVec2& aA, const ImVec2& aB) {
    const ImVec2 seg = aB - aA;
    const ImVec2 toPt = aPt - aA;
    const float len2 = seg.x * seg.x + seg.y * seg.y;
    float u = 0.0f;
    if (len2 > 0.0f) {
        u = (toPt.x * seg.x + toPt.y * seg.y) / len2;
        if (u < 0.0f)
            u = 0.0f;
        else if (u > 1.0f)
            u = 1.0f;
    }
    const ImVec2 proj = aA + seg * u;
    const ImVec2 delta = aPt - proj;
    return delta.x * delta.x + delta.y * delta.y;
}

// Cubic-Bezier control points from two anchor points + outward tangents.
// Control point distance scales with the separation so the curve "breathes"
// nicely when slots are far/close.
inline void s_bezierCtrl(const ImVec2& aFrom, const ImVec2& aFromTangent, const ImVec2& aTo, const ImVec2& aToTangent, ImVec2& arP1, ImVec2& arP2) {
    const float dx = aTo.x - aFrom.x;
    const float dy = aTo.y - aFrom.y;
    float d = std::sqrt(dx * dx + dy * dy) * 0.4f;
    if (d < 30.0f)
        d = 30.0f;
    if (d > 200.0f)
        d = 200.0f;
    arP1 = aFrom + aFromTangent * d;
    arP2 = aTo + aToTangent * d;
}

// Resolve a slot's tangent. InOut slots use a sentinel (0,0) and get a tangent
// computed dynamically from the direction toward the other endpoint.
static ImVec2 s_resolveTangent(const SlotState& arSlot, const ImVec2& aOtherPos) {
    if (arSlot.tangent.x == 0.0f && arSlot.tangent.y == 0.0f) {
        // InOut sentinel : we adopt a pure HORIZONTAL tangent, with sign
        // determined by the direction toward the other endpoint. This is
        // what makes splines passing through a reroute keep the look of
        // standard wires (Input -> tangent -1,0 ; Output -> +1,0) instead
        // of pointing diagonally when the reroute is vertically offset.
        const float dx = aOtherPos.x - arSlot.screenPos.x;
        return ImVec2(dx >= 0.0f ? 1.0f : -1.0f, 0.0f);
    }
    return arSlot.tangent;
}

static void s_drawBezierLink(
    ImDrawList* apDrawList,
    const ImVec2& aFrom,
    const ImVec2& aFromTangent,
    const ImVec2& aTo,
    const ImVec2& aToTangent,
    ImU32 aColor,
    float aThickness) {
    ImVec2 p1, p2;
    s_bezierCtrl(aFrom, aFromTangent, aTo, aToTangent, p1, p2);
    apDrawList->AddBezierCubic(aFrom, p1, p2, aTo, aColor, aThickness);
}

}  // namespace

// =====================================================================
// Custom links — low-level primitives (drawing + hit-test combined)
// =====================================================================

IMNODAL_API bool BeginLink(Id aLinkId, Id aFromSlotId, Id aToSlotId, float aThickness, ImU32 aColor) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(aLinkId != 0 && "Link id must be non-zero");
    IM_ASSERT(rCtx.Canvas().currentLinkId == 0 && "BeginLink called inside another BeginLink/EndLink scope");

    auto itF = rCtx.slots.find(aFromSlotId);
    auto itT = rCtx.slots.find(aToSlotId);
    if (itF == rCtx.slots.end() || itT == rCtx.slots.end()) {
        return false;
    }
    SlotState& rFrom = itF->second;
    SlotState& rTo = itT->second;

    LinkState& rLink = rCtx.links[aLinkId];
    rLink.fromSlot = aFromSlotId;
    rLink.toSlot = aToSlotId;
    rLink.graphId = rCtx.Graph().id;
    if (aThickness <= 0.0f)
        aThickness = rCtx.style.LinkThickness;
    rLink.thickness = aThickness;
    const ImU32 baseColor = (aColor != 0) ? aColor : rCtx.style.Colors[ImNodalCol_Link];
    rLink.color = baseColor;

    rFrom.connected = true;
    rTo.connected = true;

    // Resolve tangents (InOut sentinel (0,0) -> horizontal, sign from chord).
    const ImVec2 fromTan = s_resolveTangent(rFrom, rTo.screenPos);
    const ImVec2 toTan = s_resolveTangent(rTo, rFrom.screenPos);

    // Reset path accumulator.
    rLink.cachedPath.clear();
    rLink.pathCached = false;

    // Open the BeginLink scope.
    const float canvasScale = rCtx.Canvas().scale > 0.0f ? rCtx.Canvas().scale : 1.0f;
    rCtx.Canvas().currentLinkId = aLinkId;
    rCtx.Canvas().currentLinkBaseColor = baseColor;
    rCtx.Canvas().currentLinkThickness = aThickness;
    rCtx.Canvas().currentLinkHitThreshold = ImMax(aThickness * 2.0f, 6.0f / canvasScale);
    rCtx.Canvas().currentLinkHovered = false;
    rCtx.Canvas().currentLinkFromPos = rFrom.screenPos;
    rCtx.Canvas().currentLinkToPos = rTo.screenPos;
    rCtx.Canvas().currentLinkFromTangent = fromTan;
    rCtx.Canvas().currentLinkToTangent = toTan;

    // Pick the draw list once. Channel switch happens here (inside graph) or
    // not at all (standalone, draws on the window list).
    if ((rCtx.pGraph != nullptr)) {
        rCtx.Canvas().currentLinkDrawList = rCtx.Canvas().drawList;
        s_setChannel(rCtx, GC_Links);
    } else {
        rCtx.Canvas().currentLinkDrawList = ImGui::GetWindowDrawList();
    }
    return true;
}

IMNODAL_API ImVec2 GetLinkFromPos() {
    Context& rCtx = s_getCtx();
    return rCtx.Canvas().currentLinkId != 0 ? rCtx.Canvas().currentLinkFromPos : ImVec2(0.0f, 0.0f);
}
IMNODAL_API ImVec2 GetLinkToPos() {
    Context& rCtx = s_getCtx();
    return rCtx.Canvas().currentLinkId != 0 ? rCtx.Canvas().currentLinkToPos : ImVec2(0.0f, 0.0f);
}
IMNODAL_API ImVec2 GetLinkFromTangent() {
    Context& rCtx = s_getCtx();
    return rCtx.Canvas().currentLinkId != 0 ? rCtx.Canvas().currentLinkFromTangent : ImVec2(0.0f, 0.0f);
}
IMNODAL_API ImVec2 GetLinkToTangent() {
    Context& rCtx = s_getCtx();
    return rCtx.Canvas().currentLinkId != 0 ? rCtx.Canvas().currentLinkToTangent : ImVec2(0.0f, 0.0f);
}

IMNODAL_API void SetLinkHitThickness(float aPixelThreshold) {
    Context& rCtx = s_getCtx();
    if (rCtx.Canvas().currentLinkId == 0)
        return;
    if (aPixelThreshold > 0.0f)
        rCtx.Canvas().currentLinkHitThreshold = aPixelThreshold;
}

IMNODAL_API void LinkLineSegment(const ImVec2& aP0, const ImVec2& aP1) {
    Context& rCtx = s_getCtx();
    if (rCtx.Canvas().currentLinkId == 0)
        return;
    LinkState& rLink = rCtx.links[rCtx.Canvas().currentLinkId];
    // Push p0 only on the very first segment so consecutive primitives form
    // a continuous polyline (each segment's end becomes the next's start).
    if (rLink.cachedPath.empty())
        rLink.cachedPath.push_back(aP0);
    rLink.cachedPath.push_back(aP1);

    // Canvas-hovered gate : a link never registers as hovered when the mouse
    // sits on a side panel, another window or another canvas — even if its
    // canvas-space coordinates happen to align with the cursor through pan/zoom.
    // Outside a canvas (rare standalone use), no gating.
    const bool gateOK = (!rCtx.Canvas().active) || rCtx.Canvas().hovered;
    if (!gateOK)
        return;
    const float d2 = s_pointToSegmentDistanceSq(ImGui::GetIO().MousePos, aP0, aP1);
    const float thr = rCtx.Canvas().currentLinkHitThreshold;
    if (d2 <= thr * thr)
        rCtx.Canvas().currentLinkHovered = true;
}

IMNODAL_API void LinkBezierSegment(const ImVec2& aP0, const ImVec2& aP1, const ImVec2& aFromTangent, const ImVec2& aToTangent, int aSegments) {
    Context& rCtx = s_getCtx();
    if (rCtx.Canvas().currentLinkId == 0)
        return;
    ImVec2 cp1, cp2;
    s_bezierCtrl(aP0, aFromTangent, aP1, aToTangent, cp1, cp2);
    const int N = (aSegments > 0) ? aSegments : 24;
    ImVec2 prev = aP0;
    for (int i = 1; i <= N; ++i) {
        const float t = (float)i / (float)N;
        const float u = 1.0f - t;
        const ImVec2 pt = aP0 * (u * u * u) + cp1 * (3.0f * u * u * t) + cp2 * (3.0f * u * t * t) + aP1 * (t * t * t);
        LinkLineSegment(prev, pt);
        prev = pt;
    }
}

IMNODAL_API void LinkPolyline(const ImVec2* apPoints, int aCount) {
    Context& rCtx = s_getCtx();
    if (rCtx.Canvas().currentLinkId == 0 || apPoints == nullptr || aCount < 2)
        return;
    for (int i = 1; i < aCount; ++i) {
        LinkLineSegment(apPoints[i - 1], apPoints[i]);
    }
}

IMNODAL_API void EndLink() {
    Context& rCtx = s_getCtx();
    if (rCtx.Canvas().currentLinkId == 0)
        return;
    const Id linkId = rCtx.Canvas().currentLinkId;
    LinkState& rLink = rCtx.links[linkId];

    // Finalize hovered state from the OR-accumulator.
    rLink.hovered = rCtx.Canvas().currentLinkHovered;
    rLink.pathCached = !rLink.cachedPath.empty();

    // NB : we do NOT reset clicked / doubleClicked here. NewFrame handles
    // that at the start of every frame. If several Link() calls share the
    // same id (= visual merging), the FIRST call detects the click and sets
    // it to true ; subsequent calls don't re-enter canConsume (clickConsumed
    // is already true) and would overwrite the state to false if we reset here.
    if (rLink.hovered) {
        rCtx.Graph().currentHoveredLink = linkId;
    }

    GraphState* pGraph = (rCtx.pGraph != nullptr) ? &rCtx.Graph() : nullptr;
    const bool clickConsumed = pGraph ? pGraph->clickConsumedThisFrame : false;
    const bool canConsume = rLink.hovered && !ImGui::IsAnyItemHovered() && !clickConsumed && rCtx.Graph().draggingFromSlot == 0;
    if (canConsume) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            rLink.clicked = true;
            if (pGraph) {
                const bool toggle = s_multiSelectHeld(*pGraph);
                s_selectLink(*pGraph, linkId, toggle);
                pGraph->clickConsumedThisFrame = true;
            } else {
                rCtx.Canvas().standaloneSelectedLink = linkId;
            }
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            rLink.doubleClicked = true;
            if (pGraph)
                pGraph->clickConsumedThisFrame = true;
        }
        if (ImGui::IsMouseClicked(rCtx.Canvas().settings.contextMenuButton)) {
            rLink.ctxMenuRequested = true;
            rCtx.Graph().ctxMenuLinkId = linkId;
        }
    }
    if (pGraph) {
        rLink.selected = (pGraph->selectedLinks.count(linkId) > 0);
    } else {
        rLink.selected = (rCtx.Canvas().standaloneSelectedLink == linkId);
    }

    // Resolve final color and emit a single AddPolyline. Joining the segments
    // gives nicer junctions than N independent AddLine calls.
    ImU32 drawColor = rCtx.Canvas().currentLinkBaseColor;
    if (rLink.selected) {
        drawColor = rCtx.style.Colors[ImNodalCol_LinkSelected];
    } else if (rLink.hovered) {
        drawColor = rCtx.style.Colors[ImNodalCol_LinkHovered];
    }
    if (rLink.cachedPath.size() >= 2 && rCtx.Canvas().currentLinkDrawList != nullptr) {
        rCtx.Canvas().currentLinkDrawList->AddPolyline(rLink.cachedPath.data(), (int)rLink.cachedPath.size(), drawColor, rCtx.Canvas().currentLinkThickness, ImDrawFlags_None);
    }

    // Restore channel and close the scope.
    if ((rCtx.pGraph != nullptr)) {
        s_setChannel(rCtx, GC_Content);
    }
    rCtx.Canvas().currentLinkId = 0;
    rCtx.Canvas().currentLinkDrawList = nullptr;
}

IMNODAL_API void Link(Id aLinkId, Id aFromSlotId, Id aToSlotId, ImU32 aColor, float aThickness) {
    if (!BeginLink(aLinkId, aFromSlotId, aToSlotId, aThickness, aColor))
        return;
    LinkBezierSegment(GetLinkFromPos(), GetLinkToPos(), GetLinkFromTangent(), GetLinkToTangent(), 24);
    EndLink();
}

// =====================================================================
// Connection creation state machine (M2)
// =====================================================================

IMNODAL_API bool BeginConnectionCreate() {
    Context& rCtx = s_getCtx();
    // True only when a drag is active AND it originated from the currently
    // open scope (graph or standalone). Without this check, a drag started in
    // one scope would trigger query/commit in every other scope too.
    bool ret = s_isDragInCurrentScope(rCtx);
    return ret;
}
IMNODAL_API Id GetDraggingFromSlot() {
    Context& rCtx = s_getCtx();
    return rCtx.pGraph != nullptr ? rCtx.pGraph->draggingFromSlot : (Id)0;
}
IMNODAL_API bool QueryNewLink(Id* apoFromSlotId, Id* apoToSlotId) {
    Context& rCtx = s_getCtx();
    if (!s_isDragInCurrentScope(rCtx))
        return false;
    const Id from = rCtx.Graph().draggingFromSlot;
    const Id to = rCtx.Graph().currentHoveredSlot;
    if (to == 0 || to == from)
        return false;
    auto itF = rCtx.slots.find(from);
    auto itT = rCtx.slots.find(to);
    if (itF == rCtx.slots.end() || itT == rCtx.slots.end())
        return false;
    // Slot-flag gate : target slot may explicitly opt out of being a link
    // sink. Source slot may opt out of being a link source.
    if (itF->second.flags & ImNodalSlotFlags_NoConnectionStart)
        return false;
    if (itT->second.flags & ImNodalSlotFlags_NoConnectionEnd)
        return false;
    // Target must live in the same scope too (no cross-scope links in M2).
    const Id currentScope = (rCtx.pGraph != nullptr) ? rCtx.Graph().id : (Id)0;
    if (itT->second.graphId != currentScope)
        return false;
    // Don't offer self-connections within the same node.
    if (itF->second.parentNode != 0 && itF->second.parentNode == itT->second.parentNode) {
        return false;
    }
    if (apoFromSlotId)
        *apoFromSlotId = from;
    if (apoToSlotId)
        *apoToSlotId = to;
    return true;
}

IMNODAL_API bool AcceptNewLink(ImU32 aColor) {
    Context& rCtx = s_getCtx();
    if (!s_isDragInCurrentScope(rCtx))
        return false;
    rCtx.Graph().connAcceptedThisFrame = true;
    rCtx.Graph().connAcceptColor = (aColor != 0) ? aColor : rCtx.style.Colors[ImNodalCol_LinkPreviewAccept];
    rCtx.Graph().connRejectReason = nullptr;
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        rCtx.Graph().connCommitThisFrame = true;
        return true;
    }
    return false;
}

IMNODAL_API void RejectNewLink(const char* aReason) {
    Context& rCtx = s_getCtx();
    if (!s_isDragInCurrentScope(rCtx))
        return;
    rCtx.Graph().connAcceptedThisFrame = false;
    rCtx.Graph().connAcceptColor = 0;
    rCtx.Graph().connRejectReason = aReason ? aReason : "invalid";
}

// True while the user is dragging a link AND no slot is under the cursor —
// i.e. they're hovering empty canvas. The host typically responds by opening
// a "create node connected to this slot" popup.
IMNODAL_API bool QueryNewNodeFromSlot(Id* apoFromSlotId) {
    Context& rCtx = s_getCtx();
    if (!s_isDragInCurrentScope(rCtx))
        return false;
    if (rCtx.Graph().currentHoveredSlot != 0)
        return false;  // a target slot is hovered → that's QueryNewLink territory
    if (apoFromSlotId)
        *apoFromSlotId = rCtx.Graph().draggingFromSlot;
    return true;
}

IMNODAL_API bool AcceptNewNodeFromSlot(ImU32 aColor) {
    Context& rCtx = s_getCtx();
    if (!s_isDragInCurrentScope(rCtx))
        return false;
    if (rCtx.Graph().currentHoveredSlot != 0)
        return false;
    rCtx.Graph().connNewNodeAcceptedThisFrame = true;
    rCtx.Graph().connAcceptColor = (aColor != 0) ? aColor : rCtx.style.Colors[ImNodalCol_LinkPreviewAccept];
    rCtx.Graph().connRejectReason = nullptr;
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        rCtx.Graph().connNewNodeCommitThisFrame = true;
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
        auto itF = rCtx.slots.find(rCtx.Graph().draggingFromSlot);
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
                        insideSourceNode = (mp.x >= nr.Min.x && mp.x <= nr.Max.x && mp.y >= nr.Min.y && mp.y <= nr.Max.y);
                    }
                }
            }
            // We still draw the preview if the cursor is OVER another slot
            // (typical of "drag from one slot to another inside the same node",
            // unusual but possible) — that path uses the slot's known position
            // rather than the mouse, and the preview is informative there.
            const bool hoveringOtherSlot = (rCtx.Graph().currentHoveredSlot != 0 && rCtx.Graph().currentHoveredSlot != rCtx.Graph().draggingFromSlot);

            if (!insideSourceNode || hoveringOtherSlot) {
                ImVec2 toPos;
                ImVec2 toTangent;
                if (hoveringOtherSlot) {
                    auto itT = rCtx.slots.find(rCtx.Graph().currentHoveredSlot);
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
                if (rCtx.Graph().connAcceptedThisFrame || rCtx.Graph().connNewNodeAcceptedThisFrame) {
                    color = rCtx.Graph().connAcceptColor;
                } else if (rCtx.Graph().connRejectReason != nullptr) {
                    color = rCtx.style.Colors[ImNodalCol_LinkPreviewReject];
                } else {
                    color = rCtx.style.Colors[ImNodalCol_LinkPreviewIdle];
                }

                if ((rCtx.pGraph != nullptr)) {
                    auto* const pDrawList = rCtx.Canvas().drawList;
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
        rCtx.Graph().draggingFromSlot = 0;
    }
}

// =====================================================================
// Multi-selection
// =====================================================================

IMNODAL_API int GetSelectedObjectCount() {
    Context& rCtx = s_getCtx();
    if (!(rCtx.pGraph != nullptr))
        return 0;
    GraphState& g = rCtx.Graph();
    return (int)(g.selectedNodes.size() + g.selectedLinks.size());
}

IMNODAL_API int GetSelectedNodes(Id* apoBuffer, int aCapacity) {
    Context& rCtx = s_getCtx();
    if (!(rCtx.pGraph != nullptr) || apoBuffer == nullptr || aCapacity <= 0)
        return 0;
    GraphState& g = rCtx.Graph();
    int n = 0;
    for (Id id : g.selectedNodes) {
        if (n >= aCapacity)
            break;
        apoBuffer[n++] = id;
    }
    return n;
}

IMNODAL_API int GetSelectedLinks(Id* apoBuffer, int aCapacity) {
    Context& rCtx = s_getCtx();
    if (!(rCtx.pGraph != nullptr) || apoBuffer == nullptr || aCapacity <= 0)
        return 0;
    GraphState& g = rCtx.Graph();
    int n = 0;
    for (Id id : g.selectedLinks) {
        if (n >= aCapacity)
            break;
        apoBuffer[n++] = id;
    }
    return n;
}

IMNODAL_API bool HasSelectionChanged() {
    Context& rCtx = s_getCtx();
    if (!(rCtx.pGraph != nullptr))
        return false;
    return rCtx.Graph().selectionChangedThisFrame;
}

// AddToSelection / RemoveFromSelection look up the id type — node or link —
// so the host doesn't need to disambiguate.
IMNODAL_API void AddToSelection(Id aId) {
    Context& rCtx = s_getCtx();
    IM_ASSERT((rCtx.pGraph != nullptr) && "AddToSelection must be called inside BeginGraph/EndGraph");
    if (aId == 0)
        return;
    GraphState& g = rCtx.Graph();
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
    IM_ASSERT((rCtx.pGraph != nullptr) && "RemoveFromSelection must be called inside BeginGraph/EndGraph");
    if (aId == 0)
        return;
    GraphState& g = rCtx.Graph();
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
    IM_ASSERT((rCtx.pGraph != nullptr) && "ClearSelection must be called inside BeginGraph/EndGraph");
    s_clearSelection(rCtx.Graph());
}

// =====================================================================
// Direct hover queries
// =====================================================================
IMNODAL_API Id GetHoveredSlot() {
    Context& rCtx = s_getCtx();
    return rCtx.pGraph != nullptr ? rCtx.pGraph->currentHoveredSlot : (Id)0;
}
IMNODAL_API Id GetHoveredNode() {
    Context& rCtx = s_getCtx();
    return rCtx.pGraph != nullptr ? rCtx.pGraph->currentHoveredNode : (Id)0;
}
IMNODAL_API Id GetHoveredLink() {
    Context& rCtx = s_getCtx();
    return rCtx.pGraph != nullptr ? rCtx.pGraph->currentHoveredLink : (Id)0;
}

// =====================================================================
// Context-menu requests
// =====================================================================
IMNODAL_API bool IsNodeContextMenuRequested(Id* apoNodeId) {
    Context& rCtx = s_getCtx();
    if (rCtx.pGraph == nullptr || rCtx.pGraph->ctxMenuNodeId == 0)
        return false;
    if (apoNodeId)
        *apoNodeId = rCtx.pGraph->ctxMenuNodeId;
    return true;
}
IMNODAL_API bool IsSlotContextMenuRequested(Id* apoSlotId) {
    Context& rCtx = s_getCtx();
    if (rCtx.pGraph == nullptr || rCtx.pGraph->ctxMenuSlotId == 0)
        return false;
    if (apoSlotId)
        *apoSlotId = rCtx.pGraph->ctxMenuSlotId;
    return true;
}
IMNODAL_API bool IsLinkContextMenuRequested(Id* apoLinkId) {
    Context& rCtx = s_getCtx();
    if (rCtx.pGraph == nullptr || rCtx.pGraph->ctxMenuLinkId == 0)
        return false;
    if (apoLinkId)
        *apoLinkId = rCtx.pGraph->ctxMenuLinkId;
    return true;
}

// =====================================================================
// Delete state machine
// =====================================================================
IMNODAL_API bool BeginDelete() {
    Context& rCtx = s_getCtx();
    IM_ASSERT((rCtx.pGraph != nullptr) && "BeginDelete must be called inside BeginGraph/EndGraph");
    IM_ASSERT(!rCtx.Graph().deleteScopeOpen && "BeginDelete called twice without matching EndDelete");

    // Trigger: Delete or Backspace key, while the canvas is hovered.
    GraphState& g = rCtx.Graph();
    rCtx.Graph().pendingDeleteLinks.clear();
    rCtx.Graph().pendingDeleteNodes.clear();
    rCtx.Graph().acceptedDeleteLinks.clear();
    rCtx.Graph().acceptedDeleteNodes.clear();
    rCtx.Graph().currentDeleteCandidate = 0;
    rCtx.Graph().currentDeleteKind = 0;

    const bool ctrl = ImGui::IsKeyDown(ImGuiMod_Ctrl) || ImGui::IsKeyDown(ImGuiMod_Super);
    const bool ctrlX = ctrl && ImGui::IsKeyPressed(ImGuiKey_X, false);
    // repeat=false : we do NOT want delete to fire 10x if the user holds
    // the key down. A single deletion per press.
    const bool deletePressed = ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || ctrlX;
    const bool canvasHovered = rCtx.Canvas().hovered;  // computed by EndCanvas of last frame
    // WantCaptureKeyboard : an ImGui widget (text input elsewhere, etc.) has
    // captured the keyboard. A Backspace typed there must not delete our
    // canvas selection — even if the mouse happens to hover the canvas.
    const bool kbCaptured = ImGui::GetIO().WantCaptureKeyboard;
    if (deletePressed && canvasHovered && !kbCaptured) {
        for (Id id : g.selectedLinks)
            rCtx.Graph().pendingDeleteLinks.push_back(id);
        for (Id id : g.selectedNodes)
            rCtx.Graph().pendingDeleteNodes.push_back(id);
    }

    if (rCtx.Graph().pendingDeleteLinks.empty() && rCtx.Graph().pendingDeleteNodes.empty()) {
        return false;
    }
    rCtx.Graph().deleteScopeOpen = true;
    return true;
}

IMNODAL_API bool QueryDeletedLink(Id* apoLinkId) {
    Context& rCtx = s_getCtx();
    if (!rCtx.Graph().deleteScopeOpen)
        return false;
    if (rCtx.Graph().pendingDeleteLinks.empty()) {
        rCtx.Graph().currentDeleteCandidate = 0;
        rCtx.Graph().currentDeleteKind = 0;
        return false;
    }
    rCtx.Graph().currentDeleteCandidate = rCtx.Graph().pendingDeleteLinks.front();
    rCtx.Graph().currentDeleteKind = 1;
    if (apoLinkId)
        *apoLinkId = rCtx.Graph().currentDeleteCandidate;
    return true;
}

IMNODAL_API bool QueryDeletedNode(Id* apoNodeId) {
    Context& rCtx = s_getCtx();
    if (!rCtx.Graph().deleteScopeOpen)
        return false;
    if (rCtx.Graph().pendingDeleteNodes.empty()) {
        rCtx.Graph().currentDeleteCandidate = 0;
        rCtx.Graph().currentDeleteKind = 0;
        return false;
    }
    rCtx.Graph().currentDeleteCandidate = rCtx.Graph().pendingDeleteNodes.front();
    rCtx.Graph().currentDeleteKind = 2;
    if (apoNodeId)
        *apoNodeId = rCtx.Graph().currentDeleteCandidate;
    return true;
}

IMNODAL_API bool AcceptDelete() {
    Context& rCtx = s_getCtx();
    if (!rCtx.Graph().deleteScopeOpen || rCtx.Graph().currentDeleteCandidate == 0)
        return false;
    if (rCtx.Graph().currentDeleteKind == 1) {
        rCtx.Graph().acceptedDeleteLinks.push_back(rCtx.Graph().currentDeleteCandidate);
        rCtx.Graph().pendingDeleteLinks.pop_front();
    } else if (rCtx.Graph().currentDeleteKind == 2) {
        rCtx.Graph().acceptedDeleteNodes.push_back(rCtx.Graph().currentDeleteCandidate);
        rCtx.Graph().pendingDeleteNodes.pop_front();
    }
    rCtx.Graph().currentDeleteCandidate = 0;
    rCtx.Graph().currentDeleteKind = 0;
    return true;
}

IMNODAL_API void RejectDelete() {
    Context& rCtx = s_getCtx();
    if (!rCtx.Graph().deleteScopeOpen || rCtx.Graph().currentDeleteCandidate == 0)
        return;
    if (rCtx.Graph().currentDeleteKind == 1) {
        rCtx.Graph().pendingDeleteLinks.pop_front();
    } else if (rCtx.Graph().currentDeleteKind == 2) {
        rCtx.Graph().pendingDeleteNodes.pop_front();
    }
    rCtx.Graph().currentDeleteCandidate = 0;
    rCtx.Graph().currentDeleteKind = 0;
}

IMNODAL_API void EndDelete() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Graph().deleteScopeOpen && "EndDelete without matching BeginDelete");
    // Drop accepted ids from the selection — the host will stop emitting them
    // next frame, but we want the selection state coherent immediately.
    if ((rCtx.pGraph != nullptr)) {
        GraphState& g = rCtx.Graph();
        for (Id id : rCtx.Graph().acceptedDeleteLinks)
            g.selectedLinks.erase(id);
        for (Id id : rCtx.Graph().acceptedDeleteNodes)
            g.selectedNodes.erase(id);
        if (g.lastSelectedNode != 0 && g.selectedNodes.count(g.lastSelectedNode) == 0) {
            g.lastSelectedNode = g.selectedNodes.empty() ? 0 : *g.selectedNodes.begin();
        }
        if (g.lastSelectedLink != 0 && g.selectedLinks.count(g.lastSelectedLink) == 0) {
            g.lastSelectedLink = g.selectedLinks.empty() ? 0 : *g.selectedLinks.begin();
        }
        if (!rCtx.Graph().acceptedDeleteLinks.empty() || !rCtx.Graph().acceptedDeleteNodes.empty()) {
            g.selectionChangedThisFrame = true;
        }
    }
    // Drop accepted entries from the persistent context maps. Without this, a
    // deleted node's NodeState lingers in rCtx.nodes — its lastScreenRect is
    // frozen at the last drawn frame, so NavigateToContent (graph bbox) and
    // box-select still see the ghost. Same for links/slots.
    for (Id id : rCtx.Graph().acceptedDeleteLinks) {
        rCtx.links.erase(id);
    }
    for (Id id : rCtx.Graph().acceptedDeleteNodes) {
        // Erase all slots whose parent is this node.
        for (auto it = rCtx.slots.begin(); it != rCtx.slots.end();) {
            if (it->second.parentNode == id) {
                it = rCtx.slots.erase(it);
            } else {
                ++it;
            }
        }
        rCtx.nodes.erase(id);
    }
    rCtx.Graph().pendingDeleteLinks.clear();
    rCtx.Graph().pendingDeleteNodes.clear();
    rCtx.Graph().acceptedDeleteLinks.clear();
    rCtx.Graph().acceptedDeleteNodes.clear();
    rCtx.Graph().currentDeleteCandidate = 0;
    rCtx.Graph().currentDeleteKind = 0;
    rCtx.Graph().deleteScopeOpen = false;
}

// =====================================================================
// Shortcut machine
// =====================================================================

namespace {

// Snapshot the current selection into the action context arrays. Used as the
// "what does this shortcut act on?" payload.
static void s_captureActionContext(Context& arCtx) {
    arCtx.Graph().actionContextNodes.clear();
    arCtx.Graph().actionContextLinks.clear();
    if (!(arCtx.pGraph != nullptr))
        return;
    GraphState& g = arCtx.Graph();
    arCtx.Graph().actionContextNodes.reserve(g.selectedNodes.size());
    arCtx.Graph().actionContextLinks.reserve(g.selectedLinks.size());
    for (Id id : g.selectedNodes)
        arCtx.Graph().actionContextNodes.push_back(id);
    for (Id id : g.selectedLinks)
        arCtx.Graph().actionContextLinks.push_back(id);
}

}  // namespace

IMNODAL_API bool BeginShortcut() {
    Context& rCtx = s_getCtx();
    IM_ASSERT((rCtx.pGraph != nullptr) && "BeginShortcut must be called inside BeginGraph/EndGraph");
    IM_ASSERT(!rCtx.Graph().shortcutScopeOpen && "BeginShortcut called twice without matching EndShortcut");

    // Read shortcut keys only when the canvas is hovered — otherwise typing
    // Ctrl+C in a text input elsewhere would fire the graph's copy.
    if (!rCtx.Canvas().hovered)
        return false;

    const bool ctrl = ImGui::IsKeyDown(ImGuiMod_Ctrl) || ImGui::IsKeyDown(ImGuiMod_Super);
    if (!ctrl)
        return false;

    // Detect each shortcut once per press.
    rCtx.Graph().shortcutCopyFired = ImGui::IsKeyPressed(ImGuiKey_C, false);
    rCtx.Graph().shortcutPasteFired = ImGui::IsKeyPressed(ImGuiKey_V, false);
    rCtx.Graph().shortcutCutFired = ImGui::IsKeyPressed(ImGuiKey_X, false);
    rCtx.Graph().shortcutDuplicateFired = ImGui::IsKeyPressed(ImGuiKey_D, false);
    rCtx.Graph().shortcutSelectAllFired = ImGui::IsKeyPressed(ImGuiKey_A, false);

    const bool any =
        rCtx.Graph().shortcutCopyFired || rCtx.Graph().shortcutPasteFired || rCtx.Graph().shortcutCutFired || rCtx.Graph().shortcutDuplicateFired || rCtx.Graph().shortcutSelectAllFired;
    if (!any)
        return false;

    // Select-all is a host-side operation but we can do it ourselves: insert
    // every node and every link of the current graph into the selection.
    if (rCtx.Graph().shortcutSelectAllFired) {
        GraphState& g = rCtx.Graph();
        const size_t before = g.selectedNodes.size() + g.selectedLinks.size();
        for (const auto& kv : rCtx.nodes) {
            if (kv.second.graphId == g.id)
                g.selectedNodes.insert(kv.first);
        }
        for (const auto& kv : rCtx.links) {
            if (kv.second.graphId == g.id)
                g.selectedLinks.insert(kv.first);
        }
        if (g.selectedNodes.size() + g.selectedLinks.size() != before) {
            g.selectionChangedThisFrame = true;
        }
    }

    s_captureActionContext(rCtx);
    rCtx.Graph().shortcutScopeOpen = true;
    return true;
}

IMNODAL_API bool AcceptCopy() {
    return s_getCtx().Graph().shortcutCopyFired;
}
IMNODAL_API bool AcceptPaste() {
    return s_getCtx().Graph().shortcutPasteFired;
}
IMNODAL_API bool AcceptCut() {
    return s_getCtx().Graph().shortcutCutFired;
}
IMNODAL_API bool AcceptDuplicate() {
    return s_getCtx().Graph().shortcutDuplicateFired;
}
IMNODAL_API bool AcceptSelectAll() {
    return s_getCtx().Graph().shortcutSelectAllFired;
}

IMNODAL_API void EndShortcut() {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Graph().shortcutScopeOpen && "EndShortcut without matching BeginShortcut");
    rCtx.Graph().shortcutScopeOpen = false;
    // Action-context arrays are kept around for queries this frame; they
    // are reset by the next NewFrame (or the next BeginShortcut).
}

IMNODAL_API int GetActionContextNodes(Id* apoBuffer, int aCapacity) {
    Context& rCtx = s_getCtx();
    if (apoBuffer == nullptr || aCapacity <= 0)
        return 0;
    const int n = ImMin((int)rCtx.Graph().actionContextNodes.size(), aCapacity);
    for (int i = 0; i < n; ++i)
        apoBuffer[i] = rCtx.Graph().actionContextNodes[i];
    return n;
}

IMNODAL_API int GetActionContextLinks(Id* apoBuffer, int aCapacity) {
    Context& rCtx = s_getCtx();
    if (apoBuffer == nullptr || aCapacity <= 0)
        return 0;
    const int n = ImMin((int)rCtx.Graph().actionContextLinks.size(), aCapacity);
    for (int i = 0; i < n; ++i)
        apoBuffer[i] = rCtx.Graph().actionContextLinks[i];
    return n;
}

// =====================================================================
// Flow animation on a link
// =====================================================================
//
// The flow is rendered as N small dots travelling along the link's path.
// We walk the cachedPath captured by Link()/EndLink() this frame — that's
// why FlowLink() must be called AFTER the matching Link()/EndLink() call.
// Since the path is a polyline, we step it by arc-length and lerp between
// vertices: works for cubic bezier samples, Manhattan, or any custom shape.

IMNODAL_API void FlowLink(Id aLinkId, float aSpeed, ImU32 aColor) {
    Context& rCtx = s_getCtx();
    auto it = rCtx.links.find(aLinkId);
    if (it == rCtx.links.end())
        return;
    LinkState& rLink = it->second;
    if (!rLink.pathCached || rLink.cachedPath.size() < 2)
        return;  // Link() wasn't called this frame, or path is degenerate

    constexpr int kDotCount = 5;
    const auto& pts = rLink.cachedPath;
    const int n = (int)pts.size();

    // Cumulative arc-length along the polyline.
    std::vector<float> cum;
    cum.resize(n);
    cum[0] = 0.0f;
    for (int i = 1; i < n; ++i) {
        const ImVec2 d = pts[i] - pts[i - 1];
        cum[i] = cum[i - 1] + std::sqrt(d.x * d.x + d.y * d.y);
    }
    const float totalLen = cum.back() + 1e-3f;

    // Phase progresses every frame, wraps over [0, 1).
    const float t = static_cast<float>(ImGui::GetTime());
    const float canvasScale = rCtx.Canvas().scale > 0.0f ? rCtx.Canvas().scale : 1.0f;
    const float phase = std::fmod(t * aSpeed * canvasScale / totalLen, 1.0f);

    const ImU32 color = (aColor != 0) ? aColor : rCtx.style.Colors[ImNodalCol_FlowDot];
    const float dotRadius = ImMax(rLink.thickness * 0.9f, 2.0f);

    auto* const pDrawList = (rCtx.pGraph != nullptr) ? rCtx.Canvas().drawList : ImGui::GetWindowDrawList();
    if ((rCtx.pGraph != nullptr))
        s_setChannel(rCtx, GC_Links);

    for (int i = 0; i < kDotCount; ++i) {
        float u = phase + (float)i / (float)kDotCount;
        if (u >= 1.0f)
            u -= 1.0f;
        const float target = u * totalLen;
        // Find segment idx where cum[idx] <= target <= cum[idx+1].
        int seg = 0;
        for (int s = 1; s < n; ++s) {
            if (cum[s] >= target) {
                seg = s - 1;
                break;
            }
            seg = s - 1;
        }
        const float segLen = cum[seg + 1] - cum[seg];
        const float local = (segLen > 0.0f) ? (target - cum[seg]) / segLen : 0.0f;
        const ImVec2 pt = pts[seg] * (1.0f - local) + pts[seg + 1] * local;
        pDrawList->AddCircleFilled(pt, dotRadius, color);
    }

    if ((rCtx.pGraph != nullptr))
        s_setChannel(rCtx, GC_Content);
}

// =====================================================================
// MiniMap
// =====================================================================
// Floating overview anchored to a corner of the canvas. Acts like a
// modal-ish window over the graph : while the cursor is over its rect,
// rCtx.Canvas().hovered is forced false (gate set in BeginCanvas the next frame),
// so node/slot/link hit-tests, pan, box-select and bg menu are all blocked.
// Drag inside the minimap recenters the canvas on the pointed point
// (UE-style). Wheel zooms anchored on that same point.
//
// Coordinate model : we work in screen-space for layout/interactions
// (mmScreen, mousePosBackup), then convert to canvas-space when feeding
// the drawList because BeginCanvas is in local-space (drawList applies
// screen = canvas * scale + viewTransformPos). Line thickness is also
// scaled by invScale so the on-screen pixel size stays constant.

IMNODAL_API void ShowMiniMap(const MiniMapSettings& arSettings) {
    Context& rCtx = s_getCtx();
    if (!rCtx.Canvas().active || !(rCtx.pGraph != nullptr))
        return;
    if (arSettings.size.x <= 0.0f || arSettings.size.y <= 0.0f)
        return;

    const Style& s = rCtx.style;

    // Match the minimap's aspect ratio to the canvas widget's. arSettings.size
    // is the MAX bounding box budget ; we shrink the dimension that doesn't
    // fit the canvas aspect. With matching aspects, projecting the widgetRect
    // into the minimap is a clean uniform scale (no axis distortion, no
    // precision bugs from independent x/y fits).
    const ImVec2 wp = rCtx.Canvas().widgetPos;
    const ImVec2 ws = rCtx.Canvas().widgetSize;
    ImVec2 mmSize = arSettings.size;
    if (ws.x > 0.0f && ws.y > 0.0f && mmSize.x > 0.0f && mmSize.y > 0.0f) {
        const float canvasAspect = ws.y / ws.x;
        const float mmAspect = mmSize.y / mmSize.x;
        if (canvasAspect <= mmAspect) {
            mmSize.y = mmSize.x * canvasAspect;
        } else {
            mmSize.x = mmSize.y / canvasAspect;
        }
    }

    // Anchor the minimap rect to a corner of the canvas widget.
    ImVec2 mmTL;
    switch (arSettings.anchor) {
        case ImNodalCorner_TopLeft:
            mmTL = ImVec2(wp.x + arSettings.offset.x, wp.y + arSettings.offset.y);
            break;
        case ImNodalCorner_BottomLeft:
            mmTL = ImVec2(wp.x + arSettings.offset.x, wp.y + ws.y - mmSize.y - arSettings.offset.y);
            break;
        case ImNodalCorner_BottomRight:
            mmTL = ImVec2(wp.x + ws.x - mmSize.x - arSettings.offset.x,
                          wp.y + ws.y - mmSize.y - arSettings.offset.y);
            break;
        case ImNodalCorner_TopRight:
        default:
            mmTL = ImVec2(wp.x + ws.x - mmSize.x - arSettings.offset.x,
                          wp.y + arSettings.offset.y);
            break;
    }
    const ImRect mmScreen(mmTL, mmTL + mmSize);
    rCtx.Canvas().lastMiniMapRect = mmScreen;

    // CRITICAL : NodeState::lastScreenRect is misnamed — it's stored in
    // CANVAS-SPACE (captured by EndNode while inside the local-space scope).
    // widgetRect, mmScreen and mousePosBackup are in SCREEN-SPACE. We work
    // in screen-space throughout and convert nodes via canvas→screen up-front
    // (otherwise the projection compares two different spaces and the
    // viewport rect doesn't line up with the visible nodes).
    auto canvasToScreen = [&](const ImVec2& p) -> ImVec2 {
        return ImVec2(p.x * rCtx.Canvas().scale + rCtx.Canvas().viewTransformPos.x,
                      p.y * rCtx.Canvas().scale + rCtx.Canvas().viewTransformPos.y);
    };

    // Build the screen-space content bbox = union of nodes EMITTED THIS
    // FRAME in the current graph. We iterate frameNodeOrder (the canonical
    // "alive this frame" set) rather than rCtx.nodes which keeps every
    // node ever seen.
    // Empty graph fallback : the widget rect itself so the minimap still
    // has SOMETHING to show.
    GraphState& rGraph = rCtx.Graph();
    ImRect bbox(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));
    int nodeCount = 0;
    for (Id nid : rGraph.frameNodeOrder) {
        auto it = rCtx.nodes.find(nid);
        if (it == rCtx.nodes.end())
            continue;
        const ImRect& nr_canvas = it->second.lastScreenRect;
        if (nr_canvas.GetWidth() <= 0.0f || nr_canvas.GetHeight() <= 0.0f)
            continue;
        const ImVec2 nMin = canvasToScreen(nr_canvas.Min);
        const ImVec2 nMax = canvasToScreen(nr_canvas.Max);
        if (nMin.x < bbox.Min.x) bbox.Min.x = nMin.x;
        if (nMin.y < bbox.Min.y) bbox.Min.y = nMin.y;
        if (nMax.x > bbox.Max.x) bbox.Max.x = nMax.x;
        if (nMax.y > bbox.Max.y) bbox.Max.y = nMax.y;
        ++nodeCount;
    }
    if (nodeCount == 0) {
        bbox = rCtx.Canvas().widgetRect;
    }
    bbox.Expand(40.0f);

    // Fit the bbox inside the minimap, preserving aspect ratio.
    const ImVec2 bSize = bbox.Max - bbox.Min;
    if (bSize.x <= 0.0f || bSize.y <= 0.0f) {
        return;  // degenerate
    }
    const float fitX = mmSize.x / bSize.x;
    const float fitY = mmSize.y / bSize.y;
    const float fit = ImMin(fitX, fitY);
    const ImVec2 contentSize(bSize.x * fit, bSize.y * fit);
    const ImVec2 mmContentTL(mmTL.x + (mmSize.x - contentSize.x) * 0.5f,
                             mmTL.y + (mmSize.y - contentSize.y) * 0.5f);

    auto screenToMM = [&](const ImVec2& p) -> ImVec2 {
        return ImVec2(mmContentTL.x + (p.x - bbox.Min.x) * fit,
                      mmContentTL.y + (p.y - bbox.Min.y) * fit);
    };
    auto mmToScreen = [&](const ImVec2& mp) -> ImVec2 {
        return ImVec2(bbox.Min.x + (mp.x - mmContentTL.x) / fit,
                      bbox.Min.y + (mp.y - mmContentTL.y) / fit);
    };

    // Interactions : mouse positions in screen-space (mousePosBackup is the
    // raw screen coord saved before s_enterLocalSpace).
    const ImVec2 mouseScreen = rCtx.Canvas().mousePosBackup;
    const bool overMiniMap = mmScreen.Contains(mouseScreen);

    const bool lmbClicked  = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool lmbDown     = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool lmbReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);

    // Update the gate flags read by the next BeginCanvas (1-frame lag is
    // imperceptible at 60 fps and avoids ordering constraints between
    // ShowMiniMap and the node/link emission inside the same scope).
    rCtx.Canvas().minimapHovered = overMiniMap;

    if (overMiniMap && lmbClicked) {
        rCtx.Canvas().minimapActive = true;
    }
    // SetCanvasView/CenterCanvasOn internally call s_leaveLocalSpace which
    // asserts the splitter is on `expectedChannel`. Since we're inside a
    // BeginGraph scope (splitter active, current = GC_Content), the assert
    // would fire. Save the current channel, switch to expectedChannel for
    // the call, then restore. pGraph is always non-null here (we returned
    // earlier otherwise), so the splitter is always active.
    auto* const dlForView = rCtx.Canvas().drawList;
    auto withExpectedChannel = [&](auto&& fn) {
        const int saved = dlForView->_Splitter._Current;
        if (saved != rCtx.Canvas().expectedChannel)
            dlForView->ChannelsSetCurrent(rCtx.Canvas().expectedChannel);
        fn();
        if (saved != dlForView->_Splitter._Current)
            dlForView->ChannelsSetCurrent(saved);
    };
    if (rCtx.Canvas().minimapActive) {
        if (lmbDown) {
            // UE-style : recenter the canvas so the screen point under the
            // cursor (= the equivalent canvas point) becomes the widget
            // center. Keeps the current scale.
            const ImVec2 targetScreen = mmToScreen(mouseScreen);
            const ImVec2 canvasP = (targetScreen - rCtx.Canvas().viewTransformPos) * rCtx.Canvas().invScale;
            withExpectedChannel([&]{ CenterCanvasOn(canvasP); });
        }
        if (lmbReleased) {
            rCtx.Canvas().minimapActive = false;
        }
    }
    if (overMiniMap) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const float newScale = ImClamp(rCtx.Canvas().scale + wheel * rCtx.Canvas().settings.zoomStep,
                                           rCtx.Canvas().settings.zoomMin, rCtx.Canvas().settings.zoomMax);
            if (newScale > 0.0f && newScale != rCtx.Canvas().scale) {
                const ImVec2 targetScreen = mmToScreen(mouseScreen);
                const ImVec2 canvasP = (targetScreen - rCtx.Canvas().viewTransformPos) * rCtx.Canvas().invScale;
                const ImVec2 newOrigin = targetScreen - rCtx.Canvas().widgetPos - canvasP * newScale;
                withExpectedChannel([&]{ SetCanvasView(newOrigin, newScale); });
            }
        }
    }

    // Render on overlay channel. Coords are converted to canvas-space and
    // line thicknesses scaled by invScale so the on-screen pixel size is
    // preserved regardless of zoom.
    auto* const dl = rCtx.Canvas().drawList;
    s_setChannel(rCtx, GC_Overlay);

    auto sc = [&](const ImVec2& p) -> ImVec2 {
        return (p - rCtx.Canvas().viewTransformPos) * rCtx.Canvas().invScale;
    };
    const float lineSc = rCtx.Canvas().invScale;

    const ImU32 bgCol = arSettings.bgColor != 0 ? arSettings.bgColor : s.Colors[ImNodalCol_MiniMapBg];
    const ImU32 brCol = arSettings.borderColor != 0 ? arSettings.borderColor : s.Colors[ImNodalCol_MiniMapBorder];
    const ImU32 vpCol = arSettings.viewportRectColor != 0 ? arSettings.viewportRectColor : s.Colors[ImNodalCol_MiniMapViewport];
    const ImU32 nodeFallback = s.Colors[ImNodalCol_MiniMapNode];

    // BG fill
    dl->AddRectFilled(sc(mmScreen.Min), sc(mmScreen.Max), bgCol);

    // Node boxes (no slots, no titles — just the rectangle with the host's
    // accent color when set, else the default minimap node color). Iterate
    // frameNodeOrder so we never paint stale nodes. lastScreenRect is in
    // CANVAS-SPACE so we convert to screen-space before projecting.
    for (Id nid : rGraph.frameNodeOrder) {
        auto it = rCtx.nodes.find(nid);
        if (it == rCtx.nodes.end())
            continue;
        const NodeState& n = it->second;
        if (n.flags & ImNodalNodeFlags_HiddenInMinimap)
            continue;
        const ImRect& nr_canvas = n.lastScreenRect;
        if (nr_canvas.GetWidth() <= 0.0f)
            continue;
        const ImVec2 nMin = canvasToScreen(nr_canvas.Min);
        const ImVec2 nMax = canvasToScreen(nr_canvas.Max);
        const ImVec2 a = screenToMM(nMin);
        const ImVec2 b = screenToMM(nMax);
        const ImU32 col = n.color != 0 ? n.color : nodeFallback;
        dl->AddRectFilled(sc(a), sc(b), col);
    }

    // Viewport rect (current canvas widget projected into the minimap),
    // clipped to the minimap's own rect so it doesn't bleed outside.
    {
        ImVec2 va = screenToMM(rCtx.Canvas().widgetRect.Min);
        ImVec2 vb = screenToMM(rCtx.Canvas().widgetRect.Max);
        if (va.x < mmScreen.Min.x) va.x = mmScreen.Min.x;
        if (va.y < mmScreen.Min.y) va.y = mmScreen.Min.y;
        if (vb.x > mmScreen.Max.x) vb.x = mmScreen.Max.x;
        if (vb.y > mmScreen.Max.y) vb.y = mmScreen.Max.y;
        if (vb.x > va.x && vb.y > va.y) {
            dl->AddRect(sc(va), sc(vb), vpCol, 0.0f, arSettings.viewportRectThickness * lineSc);
        }
    }

    // Border
    dl->AddRect(sc(mmScreen.Min), sc(mmScreen.Max), brCol, 0.0f, arSettings.borderThickness * lineSc);

    s_setChannel(rCtx, GC_Content);
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
    if (!(rCtx.pGraph != nullptr))
        return ImGui::GetWindowDrawList();
    if (rCtx.nodes.count(aNodeId) == 0)
        return rCtx.Canvas().drawList;
    s_setChannel(rCtx, GC_UserBg);
    return rCtx.Canvas().drawList;
}

IMNODAL_API ImDrawList* GetNodeForegroundDrawList(Id aNodeId) {
    Context& rCtx = s_getCtx();
    if (!(rCtx.pGraph != nullptr))
        return ImGui::GetWindowDrawList();
    if (rCtx.nodes.count(aNodeId) == 0)
        return rCtx.Canvas().drawList;
    s_setChannel(rCtx, GC_UserFg);
    return rCtx.Canvas().drawList;
}

IMNODAL_API ImRect GetNodeRect(Id aNodeId) {
    Context& rCtx = s_getCtx();
    auto it = rCtx.nodes.find(aNodeId);
    if (it == rCtx.nodes.end())
        return ImRect();
    const ImRect& r = it->second.lastScreenRect;
    // The cached rect is in LOCAL space (captured during EndNode while inside
    // BeginCanvas local-space scope). When the caller is outside that scope
    // — i.e. canvas inactive or suspended — translate to screen so the rect
    // is directly usable for ImGui screen-space draws / popups.
    const bool inLocal = rCtx.Canvas().active && rCtx.Canvas().suspendCounter == 0;
    if (inLocal)
        return r;
    return ImRect(s_canvasToScreen(rCtx, r.Min), s_canvasToScreen(rCtx, r.Max));
}

// =====================================================================
// Slot dot pivot
// =====================================================================
IMNODAL_API void SlotAlignment(const ImVec2& aAlignment) {
    s_getCtx().Canvas().slotAlignment = aAlignment;
}
IMNODAL_API void SlotSize(const ImVec2& aSizePx) {
    s_getCtx().Canvas().slotSize = aSizePx;
}
IMNODAL_API void SlotPivotOffset(const ImVec2& aOffsetPx) {
    s_getCtx().Canvas().slotPivotOffset = aOffsetPx;
}
IMNODAL_API void SetSlotAnchor(const ImVec2& aScreenPos) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Graph().currentSlotId != 0 && "SetSlotAnchor must be called between BeginSlot/EndSlot");
    rCtx.Canvas().slotAnchorSet = true;
    rCtx.Canvas().slotAnchorPos = aScreenPos;
}
IMNODAL_API void SetSlotTangent(const ImVec2& aDirection) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Graph().currentSlotId != 0 && "SetSlotTangent must be called between BeginSlot/EndSlot");
    rCtx.Canvas().slotTangentSet = true;
    rCtx.Canvas().slotTangentDir = aDirection;
}

// =====================================================================
// Custom hitbox API
// =====================================================================
// SetSlotHitbox / SetNodeHitbox stage a shape that End* consumes to
// override the rectangular hit area. Convex polygons are validated here
// once per call (debug assert) — never per hit-test.

IMNODAL_API void SetSlotHitbox(const ImNodalShape& aHitbox) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Graph().currentSlotId != 0 && "SetSlotHitbox must be called between BeginSlot/EndSlot");
    if (aHitbox.type == ImNodalHitShape_ConvexPolygon) {
        IM_ASSERT(s_isConvexPolygon(aHitbox.polygonPoints, aHitbox.polygonCount) &&
                  "ImNodal hitbox polygon must be convex (and have >= 3 distinct points)");
    }
    rCtx.Canvas().stagingSlotHitbox = aHitbox;
}

IMNODAL_API void SetNodeHitbox(const ImNodalShape& aHitbox) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Graph().currentNodeId != 0 && "SetNodeHitbox must be called between BeginNode/EndNode");
    if (aHitbox.type == ImNodalHitShape_ConvexPolygon) {
        IM_ASSERT(s_isConvexPolygon(aHitbox.polygonPoints, aHitbox.polygonCount) &&
                  "ImNodal hitbox polygon must be convex (and have >= 3 distinct points)");
    }
    rCtx.Canvas().stagingNodeHitbox = aHitbox;
}

IMNODAL_API void SetNodeBodyShape(const ImNodalShape& aShape) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Graph().currentNodeId != 0 && "SetNodeBodyShape must be called between BeginNode/EndNode");
    if (aShape.type == ImNodalHitShape_ConvexPolygon) {
        IM_ASSERT(s_isConvexPolygon(aShape.polygonPoints, aShape.polygonCount) &&
                  "ImNodal body shape polygon must be convex (and have >= 3 distinct points)");
    }
    rCtx.Canvas().stagingNodeBodyShape = aShape;
}

IMNODAL_API void SetSlotBodyShape(const ImNodalShape& aShape) {
    Context& rCtx = s_getCtx();
    IM_ASSERT(rCtx.Graph().currentSlotId != 0 && "SetSlotBodyShape must be called between BeginSlot/EndSlot");
    if (aShape.type == ImNodalHitShape_ConvexPolygon) {
        IM_ASSERT(s_isConvexPolygon(aShape.polygonPoints, aShape.polygonCount) &&
                  "ImNodal body shape polygon must be convex (and have >= 3 distinct points)");
    }
    rCtx.Canvas().stagingSlotBodyShape = aShape;
}

// =====================================================================
// Navigation
// =====================================================================

namespace {

// Build the canvas-space bbox of a set of node ids. Returns false if no node
// in the set has a known size yet (e.g. before the first frame ran).
static bool s_collectNodeBBox(const Context& arCtx, const std::unordered_set<Id>& arSet, ImVec2& aroMin, ImVec2& aroMax) {
    bool any = false;
    for (Id id : arSet) {
        auto it = arCtx.nodes.find(id);
        if (it == arCtx.nodes.end())
            continue;
        const NodeState& n = it->second;
        if (n.size.x <= 0.0f || n.size.y <= 0.0f)
            continue;
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
    // Defensive: only count nodes that were drawn last frame. EndDelete now
    // erases entries on accepted deletion, but a host that wipes its graph
    // without going through ImNodal's delete path (e.g. clear-then-load) would
    // otherwise leave ghost NodeStates in the map and pollute the bbox.
    std::unordered_set<Id> drawnLastFrame;
    for (const auto& gkv : arCtx.graphs) {
        for (Id id : gkv.second.frameNodeOrder) {
            drawnLastFrame.insert(id);
        }
    }
    bool any = false;
    for (const auto& kv : arCtx.nodes) {
        if (!drawnLastFrame.empty() && drawnLastFrame.count(kv.first) == 0)
            continue;
        const NodeState& n = kv.second;
        if (n.size.x <= 0.0f || n.size.y <= 0.0f)
            continue;
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
    if (!s_collectAllNodesBBox(rCtx, lo, hi))
        return;
    if (aZoomToFit) {
        ZoomCanvasToRect(lo, hi, aMarginRatio);
    } else {
        const ImVec2 center((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        CenterCanvasOn(center);
    }
}

IMNODAL_API void NavigateToSelection(bool aZoomToFit, float aMarginRatio) {
    Context& rCtx = s_getCtx();
    if (!(rCtx.pGraph != nullptr))
        return;
    GraphState& g = rCtx.Graph();
    if (g.selectedNodes.empty()) {
        // Empty selection → fall back to "navigate to content".
        NavigateToContent(aZoomToFit, aMarginRatio);
        return;
    }
    ImVec2 lo, hi;
    if (!s_collectNodeBBox(rCtx, g.selectedNodes, lo, hi))
        return;
    if (aZoomToFit) {
        ZoomCanvasToRect(lo, hi, aMarginRatio);
    } else {
        const ImVec2 center((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        CenterCanvasOn(center);
    }
}

// ShowDemoWindow lives in ImNodalDemo.cpp.

}  // namespace ImNodal
