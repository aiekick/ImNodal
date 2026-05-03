# ImNodal

Zoomable / pannable node-graph primitives for Dear ImGui.

This document is the developer reference. For a runnable showcase covering
every public feature in one window, call `ImNodal::ShowDemoWindow()` (see the
[Demo window](#demo-window) section at the bottom).

---

## Design philosophy

- **The slot is the primitive.** Nodes, links and reroutes are built on top
  of slots; a slot can be emitted anywhere — inside a node section, inline
  in a window, or stand-alone.
- **`BeginSlot` / `EndSlot` and `BeginRerouteNode` / `EndRerouteNode` are
  CAPTURE-ONLY.** Same spirit as thedmd/imgui-node-editor's
  `BeginPin` / `EndPin` : ImNodal does NOT render the dot, the icon, the
  label or any padding inside the slot. The host emits its own widgets and
  paints its own dot at the pivot returned by `GetSlotScreenPos`. ImNodal
  only :
    - opens an ImGui group around the host content,
    - computes the link pivot from the resulting group rect,
    - hit-tests for hover / click / right-click,
    - paints the interactivity hover halo,
    - drives the link-drag state machine.
- **ImGui-style API.** Push / Pop styling, Begin / End scopes, `Query →
  Accept / Reject` state machines for connection creation, deletion and
  shortcuts. Settings structs carry behaviour (mouse buttons, keys); the
  `Style` struct carries appearance (colors, sizes).
- **Canvas inspired by thedmd's `ImCanvas`** — local-space transform, ImGui
  widgets scale with the zoom level.

---

## Quick start

```cpp
#include <ImNodal.h>

// 1. Once at startup
auto* ctx = ImNodal::CreateContext();
ImNodal::SetCurrentContext(ctx);
IMNODAL_CHECKVERSION();   // ABI sanity check

// 2. Each frame
ImNodal::NewFrame();

if (ImNodal::BeginCanvas("MyCanvas", ImVec2(0, 0))) {
    if (ImNodal::BeginGraph(0xCAFEull)) {
        static ImVec2 pos(100, 100);
        if (ImNodal::BeginNode(1, &pos)) {
            if (ImNodal::BeginHeader()) { ImGui::Text("Hello"); ImNodal::EndHeader(); }
            if (ImNodal::BeginInputs()) {
                if (ImNodal::BeginInputSlot(2)) { ImGui::Text("in"); ImNodal::EndSlot(); }
                ImNodal::EndInputs();
            }
            if (ImNodal::BeginOutputs()) {
                if (ImNodal::BeginOutputSlot(3)) { ImGui::Text("out"); ImNodal::EndSlot(); }
                ImNodal::EndOutputs();
            }
            ImNodal::EndNode();
        }
        ImNodal::EndGraph();
    }
    ImNodal::EndCanvas();
}

// 3. Once at shutdown
ImNodal::DestroyContext(ctx);
```

`Id` is `uint64_t`. **Ids must be non-zero and unique within the graph.**
Slots, nodes, links and reroutes share the same id space — pick any
allocation strategy you like (running counter, hash of a path, pointer cast,
your own UUID).

---

## Lifecycle

### Context

```cpp
ImNodal::Context* CreateContext();
void              DestroyContext(Context* = nullptr);  // null = current
Context*          GetCurrentContext();
void              SetCurrentContext(Context*);
```

Multiple contexts can coexist; switch with `SetCurrentContext` before each
`BeginCanvas` / `EndCanvas` block. State (style, selection, view, hover) is
per-context.

### Per-frame

```cpp
ImNodal::NewFrame();
```

Must be called once per frame **before** any other ImNodal call (same
contract as `ImGui::NewFrame`). Clears per-frame state : hover flags,
context-menu requests, stale link-drag state.

---

## Canvas

```cpp
struct CanvasSettings {
    float            zoomStep   = 0.1f;
    float            zoomMin    = 0.1f;
    float            zoomMax    = 10.0f;
    ImGuiKey         resetZoomKey       = ImGuiKey_R;             // None to disable
    ImGuiMouseButton panButton          = ImGuiMouseButton_Middle;
    ImGuiMouseButton contextMenuButton  = ImGuiMouseButton_Right;
    bool             drawGrid           = true;
};

bool BeginCanvas(const char* id, const ImVec2& size = {0,0}, const CanvasSettings& = {});
void EndCanvas();
```

`BeginCanvas` returns `false` when the widget is fully clipped — do **not**
call `EndCanvas` in that case. `aSize == (0,0)` uses the remaining content
region.

### Queries (valid during and right after `EndCanvas`)

```cpp
bool IsCanvasHovered();
bool IsCanvasBackgroundClicked();          // LMB on empty space
bool IsCanvasBackgroundDoubleClicked();
bool IsCanvasContextMenuRequested();       // right-click on empty space
bool IsCanvasPanning();
```

### View

```cpp
ImVec2 GetCanvasOrigin();   // screen pixels
float  GetCanvasScale();
void   SetCanvasView(const ImVec2& origin, float scale);
void   ResetCanvasView();
void   CenterCanvasOn(const ImVec2& canvasPoint);
void   ZoomCanvasToRect(const ImVec2& min, const ImVec2& max, float marginRatio = 0.1f);
```

### Coordinate spaces

```cpp
ImVec2 CanvasToScreen(ImVec2);    // canvas point  -> screen pixels (with origin)
ImVec2 ScreenToCanvas(ImVec2);
ImVec2 CanvasToScreenV(ImVec2);   // canvas vector -> screen vector (scale only)
ImVec2 ScreenToCanvasV(ImVec2);
```

### Suspending the canvas (overlays)

```cpp
ImNodal::SuspendCanvas();
//   ...emit minimap / HUD / debug overlay at SCREEN scale here...
ImNodal::ResumeCanvas();
```

Nestable. Useful for anything that must NOT zoom with the canvas.

---

## Graph

```cpp
struct GraphSettings {
    bool             allowBoxSelect          = true;
    bool             allowMultiSelect        = true;
    ImGuiKey         multiSelectKey          = ImGuiMod_Shift;
    ImGuiMouseButton selectButton            = ImGuiMouseButton_Left;
    ImGuiMouseButton dragButton              = ImGuiMouseButton_Left;
    float            minSlotHitRadiusScreen  = 8.0f;
};

bool BeginGraph(Id graphId, const GraphSettings& = {});
void EndGraph();
Id   GetCurrentGraphId();
```

`BeginGraph` MUST live inside a `BeginCanvas` scope.

---

## Node

```cpp
struct NodeSettings {
    bool movable          = true;
    bool hasInnerGraph    = false;
    bool drawHoverHandle  = false;   // top drag bar shown only on hover
};

bool BeginNode(Id nodeId, ImVec2* pos /* in/out, canvas space */, const NodeSettings& = {});
void EndNode();
```

`pos` is **in/out canvas-space**. When non-null and `movable`, dragging the
node updates it. Pass a pointer to your master copy.

### Per-node header tint

The default header color comes from `ImNodalCol_NodeHeader`. To override
per node, push the color before `BeginNode` :

```cpp
ImNodal::PushStyleColor(ImNodal::ImNodalCol_NodeHeader, myColor);
ImNodal::BeginNode(id, &pos);
...
ImNodal::EndNode();
ImNodal::PopStyleColor();
```

### Layout sections

All optional. Body = `Inputs | Center | Outputs` laid out as 3 columns.
Header sits above the body, Footer below.

```cpp
bool BeginHeader();   void EndHeader();
bool BeginInputs();   void EndInputs();
bool BeginCenter();   void EndCenter();
bool BeginOutputs();  void EndOutputs();
bool BeginFooter();   void EndFooter();
```

### `BeginAlign` — horizontal alignment of a row of widgets

```cpp
ImNodal::BeginAlign(0.5f);          // 0=left, 0.5=center, 1=right
ImGui::TextUnformatted("Title");
ImNodal::EndAlign();
```

Inside a node, available width auto-falls-back to last frame's node width.
**Caveat :** alignment uses the previous frame's measured width, so the
first frame is left-aligned and width changes lag by one frame.

### Per-node custom draw

Two channel-backed draw lists let you paint shapes under or over the node
content. Call them between `BeginNode` and `EndNode`, draw immediately,
do **not** emit ImGui widgets afterwards in the same scope.

```cpp
ImDrawList* GetNodeBackgroundDrawList(Id nodeId);
ImDrawList* GetNodeForegroundDrawList(Id nodeId);
ImRect      GetNodeRect(Id nodeId);   // last-frame screen-space
```

---

## Slot — capture-only primitive

```cpp
enum SlotRole_ { SlotRole_Input, SlotRole_Output, SlotRole_InOut };
struct SlotSettings { uint32_t typeTag = 0; };

bool BeginSlot       (Id, SlotRole, const SlotSettings& = {});
bool BeginInputSlot  (Id, const SlotSettings& = {});
bool BeginOutputSlot (Id, const SlotSettings& = {});
void EndSlot();
```

`BeginSlot` opens an ImGui group and `EndSlot` closes it. **ImNodal does
not render anything inside** — no dot, no label, no padding. The host owns
the visible appearance ; ImNodal owns the hit area, the link pivot and the
drag state machine.

### The recommended pattern

Reserve space, emit your widgets, end the slot, then paint your dot at the
slot pivot returned by `GetSlotScreenPos` :

```cpp
if (ImNodal::BeginInputSlot(slotId)) {
    ImGui::Dummy(ImVec2(12, 12));               // reserve room for your mark
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextUnformatted("label");
    ImNodal::EndSlot();

    const ImVec2 c = ImNodal::GetSlotScreenPos(slotId);
    const ImU32 col = ImNodal::IsSlotHovered  (slotId) ? hoverCol
                    : ImNodal::IsSlotConnected(slotId) ? connectedCol
                                                       : restCol;
    ImGui::GetWindowDrawList()->AddCircleFilled(c, 5.0f, col);
}
```

ImNodal also paints a button-style halo behind the slot rect when hovered
(colors `ImNodalCol_SlotHoverFill` / `ImNodalCol_SlotHoverBorder`) — that
is the only visual feedback ImNodal owns on a non-reroute slot.

### Default link pivot

The pivot (point where links connect) defaults to the **CENTER of the group
rect, regardless of role** — same convention as
thedmd/imgui-node-editor (`PivotAlignment = (0.5, 0.5)`,
`PivotSize = (0, 0)`). The role only drives the link tangent (Input → -X,
Output → +X, InOut → auto).

If the visible mark you paint sits on a specific edge of the content
(typical : icon at the right of an output, icon at the left of an input),
push the pivot to that edge with `SlotAlignment` (see the next section).

The hit rect equals the group rect (no inflation, since there is no dot to
extend around).

### Empty-slot fallback

If the host emits nothing between `BeginSlot` and `EndSlot`, ImNodal
substitutes a `Dummy(Style.SlotMinSize)` so the slot still has a clickable
area :

```cpp
if (ImNodal::BeginInputSlot(id)) ImNodal::EndSlot();   // minimal button

// You can still paint a dot if you want :
const ImVec2 c = ImNodal::GetSlotScreenPos(id);
ImGui::GetWindowDrawList()->AddCircleFilled(c, 5.0f, IM_COL32(255, 200, 0, 255));
```

`Style.SlotMinSize` defaults to `(12, 12)` — adjust via `PushStyleVar`.

### Slot pivot override — `SlotAlignment` / `SlotSize`

Same model as thedmd's `PinPivotAlignment` / `PinPivotSize`. Call between
`BeginSlot` and `EndSlot` ; the override applies to the current slot
only and resets to the defaults `(0.5, 0.5) / (0, 0)` at `EndSlot`.

```cpp
void SlotAlignment(const ImVec2& alignment);   // 2D position of pivot.Min
                                               //   inside the group rect.
                                               //   (0,0) = top-left,
                                               //   (1,1) = bottom-right.
void SlotSize     (const ImVec2& sizePx);      // pixel size of the pivot
                                               //   rect. (0,0) = point.
```

The link endpoint is the CENTER of the pivot rect :
`screenPos = pivot.Min + size * 0.5`. With the default `SlotSize = (0,0)`,
the pivot is a point and `screenPos = pivot.Min`.

```cpp
// Output whose icon is the rightmost element : push the pivot to the right edge.
if (BeginOutputSlot(id)) {
    SlotAlignment(ImVec2(1.0f, 0.5f));
    ImGui::TextUnformatted("name");
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(12, 12));            // <- where you'll paint the dot
    EndSlot();
}

// Pivot OUTSIDE the group rect : extend the pivot rect past the right edge
// with SlotSize, the link endpoint sits at its center.
if (BeginOutputSlot(id)) {
    SlotAlignment(ImVec2(1.0f, 0.5f));
    SlotSize     (ImVec2(16.0f, 0.0f));      // pivot rect = (right edge ... +16px)
    ImGui::TextUnformatted("name");
    EndSlot();
}
```

The link tangent comes from the role (Input → -X, Output → +X) — it is
NOT derived from the alignment.

### Slot queries

```cpp
ImVec2 GetSlotScreenPos(Id);       // pivot, screen space
ImVec2 GetSlotTangent  (Id);       // unit vector pointing away from the slot
bool   IsSlotHovered   (Id);
bool   IsSlotConnected (Id);
SlotRole GetSlotRole   (Id);
```

`IsSlotHovered` is valid **between `BeginSlot` and `EndSlot`** thanks to a
1-frame snapshot of the previous hit rect — handy for double-click
detection inside the slot block.

### Recommended-defaults colors and sizes

ImNodal does not draw the dot, but it ships with reusable defaults under
`ImNodalCol_SlotDot` / `SlotDotConnected` / `SlotDotHovered` and
`ImNodalStyleVar_SlotDotRadius`. Hosts can read them to keep a consistent
visual identity across slots :

```cpp
const ImU32 col = ImNodal::GetStyleColorU32(ImNodal::ImNodalCol_SlotDot);
const float r   = ImNodal::GetStyleVarFloat(ImNodal::ImNodalStyleVar_SlotDotRadius);
```

The reroute primitive is the only built-in helper that reads these.

---

## Links

```cpp
void Link(Id linkId, Id fromSlot, Id toSlot,
          ImU32 color = 0,             // 0 = ImNodalCol_Link
          float thickness = 3.0f);
```

Default render is a cubic Bezier whose tangents come from the two slots.
**Call `Link` after the nodes that own the slots have been emitted** —
otherwise slot screen positions are stale.

### Per-link queries

```cpp
bool IsLinkHovered      (Id);
bool IsLinkClicked      (Id, int button = 0);
bool IsLinkDoubleClicked(Id);
bool IsLinkSelected     (Id);
Id   GetSelectedLink();
void SetSelectedLink(Id /* 0 = clear */);
```

### Flow animation

Right after `Link(id, ...)`, draw moving dots along the curve :

```cpp
ImNodal::Link(linkId, from, to, color);
ImNodal::FlowLink(linkId, /*speed canvas units per second*/ 200.0f, /*color, 0=auto*/ 0);
```

---

## Connection creation (thedmd-style state machine)

Inside `BeginGraph` / `EndGraph`, **after** nodes + links are declared :

```cpp
if (ImNodal::BeginConnectionCreate()) {
    ImNodal::Id from, to;
    if (ImNodal::QueryNewLink(&from, &to)) {
        if (rules_ok(from, to)) {
            if (ImNodal::AcceptNewLink()) {        // returns true on commit (mouse release)
                myGraph.commitLink(from, to);
            }
        } else {
            ImNodal::RejectNewLink("type mismatch");
        }
    } else if (ImNodal::QueryNewNodeFromSlot(&from)) {
        // user dropped on empty canvas
        if (ImNodal::AcceptNewNodeFromSlot()) {
            ImGui::OpenPopup("create_node");
            myUI.pendingFromSlot = from;
        }
    }
    ImNodal::EndConnectionCreate();
}
```

`Begin/EndConnectionCreate` returns `true` while the user is dragging from
a slot. The Query functions tell you what the cursor is currently over.
The Accept functions paint the preview wire green and return `true` exactly
once on the commit frame. `RejectNewLink` paints it red.

---

## Delete state machine

Triggered by the Delete key (deletes the current selection). The host is
responsible for actually removing entities from its data model — simply
stop emitting them on the next frame.

```cpp
if (ImNodal::BeginDelete()) {
    ImNodal::Id id;
    while (ImNodal::QueryDeletedLink(&id)) {
        if (allow_link_delete(id)) ImNodal::AcceptDelete();
        else                       ImNodal::RejectDelete();
    }
    while (ImNodal::QueryDeletedNode(&id)) {
        if (allow_node_delete(id)) ImNodal::AcceptDelete();
        else                       ImNodal::RejectDelete();
    }
    ImNodal::EndDelete();
}
```

---

## Shortcut state machine (Ctrl+C/V/X/D/A)

```cpp
if (ImNodal::BeginShortcut()) {
    if      (ImNodal::AcceptCopy())       my_copy();
    else if (ImNodal::AcceptPaste())      my_paste();
    else if (ImNodal::AcceptCut())        my_cut();
    else if (ImNodal::AcceptDuplicate())  my_duplicate();
    else if (ImNodal::AcceptSelectAll())  my_select_all();
    ImNodal::EndShortcut();
}

// Scope at trigger time : the current selection.
int  GetActionContextNodes(Id* buf, int cap);
int  GetActionContextLinks(Id* buf, int cap);
```

---

## Selection

Selection is a **set of node ids and a set of link ids** per graph.
Shift + LMB toggles membership.

```cpp
int  GetSelectedObjectCount();
int  GetSelectedNodes(Id* buf, int cap);
int  GetSelectedLinks(Id* buf, int cap);
bool HasSelectionChanged();   // true on the frame the set changed

void AddToSelection     (Id);    // node OR link id, ImNodal looks it up
void RemoveFromSelection(Id);
void ClearSelection();

// Legacy single-id helpers — Get returns the "first" element, Set replaces the whole set.
bool IsNodeSelected(Id);
Id   GetSelectedNode();
void SetSelectedNode(Id /* 0 = clear */);
```

---

## Hover & context-menu queries

```cpp
Id   GetHoveredSlot();   // 0 = none
Id   GetHoveredNode();
Id   GetHoveredLink();

bool IsNodeHovered(Id* outHoveredId);
bool IsNodeDragging(Id);

// True on the frame the user right-clicked the matching item.
bool IsNodeContextMenuRequested(Id*);
bool IsSlotContextMenuRequested(Id*);
bool IsLinkContextMenuRequested(Id*);
```

Open ImGui popups from these — they're already deferred to the right phase.

---

## Reroute nodes

A reroute is a minimal pass-through node — no header / body / footer, just
one `SlotRole_InOut` slot at the node center. Capture-only, like the slot :
ImNodal sets up the geometry, sizes the hit area to a circle of
`aHitRadius` around the slot pivot, and **the host paints the visible dot
+ selection ring AFTER `EndRerouteNode`**.

```cpp
bool BeginRerouteNode(Id nodeId, Id slotId, ImVec2* pos,
                      const NodeSettings& = {},
                      float hitRadius = 5.0f);
void EndRerouteNode();
```

```cpp
ImNodal::BeginRerouteNode(nodeId, slotId, &pos);
ImNodal::EndRerouteNode();

const ImVec2 c = ImNodal::GetSlotScreenPos(slotId);
auto* dl       = ImGui::GetWindowDrawList();
const ImU32 dotCol = ImNodal::IsSlotHovered(slotId) ? IM_COL32_WHITE : myWireColor;
dl->AddCircleFilled(c, 5.0f, dotCol);
if (ImNodal::IsNodeSelected(nodeId)) {
    dl->AddCircle(c, 11.0f, IM_COL32(255, 220, 80, 255), 0, 1.5f);
}
```

Keep the radius you draw with close to `hitRadius` so the visible dot and
the clickable area match. Pair with double-click on a link to split it at
a point.

---

## Navigation helpers

```cpp
void NavigateToContent  (bool zoomToFit = true, float marginRatio = 0.10f);
void NavigateToSelection(bool zoomToFit = true, float marginRatio = 0.25f);
```

Recenter (and optionally zoom) on all nodes or only the current selection.
Call inside the `BeginCanvas` scope.

---

## Style — colors & vars

ImGui-style theming. The `Style` struct holds appearance only; behaviour
lives in `*Settings` structs.

```cpp
Style& GetStyle();
ImU32  GetStyleColorU32 (ImNodalCol);
float  GetStyleVarFloat (ImNodalStyleVar);
ImVec2 GetStyleVarVec2  (ImNodalStyleVar);

void   PushStyleColor(ImNodalCol, ImU32);
void   PopStyleColor (int count = 1);
void   PushStyleVar  (ImNodalStyleVar, float);
void   PushStyleVar  (ImNodalStyleVar, const ImVec2&);
void   PopStyleVar   (int count = 1);

const char* GetStyleColorName(ImNodalCol);
const char* GetStyleVarName  (ImNodalStyleVar);

// Drop-in editors for your settings UI :
void ShowStyleColorsEditor();
void ShowStyleVarsEditor();
```

Color enum (`ImNodalCol_*`) covers the grid lines, node body / header /
border, slot interaction halo (the only piece ImNodal paints on a slot),
recommended-default slot-dot colors (read by hosts / reroute), link states
(idle / hover / select / preview-accept / preview-reject), reroute
recommended ring colors, box selection and flow dot.

Var enum (`ImNodalStyleVar_*`) covers node rounding, border thickness,
header / body padding, column spacing, hover-handle height, recommended
slot-dot radius (read by hosts / reroute), slot min size (Dummy size for
empty slots), link thickness, grid size and grid subdivisions.

---

## Demo window

```cpp
ImNodal::ShowDemoWindow();              // always visible
ImNodal::ShowDemoWindow(&my_open_bool); // closable
```

Self-contained ImGui-style showcase, doubles as live reference for the
"capture-only" pattern. Sections :

- **Slot anatomy** — four slots showing how the host paints its dot after
  `EndSlot` : naked slot, padded label, label + inline widget, output with
  pivot override.
- **Live graph** — a working canvas with three nodes wired in series, a
  reroute (host-painted dot + selection ring), link drag-create with a
  "drop on empty → popup" handler, the Delete state machine, and a
  flowing link.
- **Style — colors / vars** — the two built-in editors.

State (positions, links) is kept in function-local statics; it survives
between frames but is not shared with your own graph. The full source is
in `ImNodalDemo.cpp` — copy/paste-friendly.
