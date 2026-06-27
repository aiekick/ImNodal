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
        ImNodal::SetNextNodePos(1, ImVec2(100, 100));  // init only — call once
        if (ImNodal::BeginNode(1)) {
            // Layout assemblé via BeginLayoutHorizontal/Vertical + LayoutSpring +
            // BeginLayoutGroup (header centré, body avec inputs gauche / outputs
            // droite, LayoutSpring pour pousser à droite).
            ImNodal::BeginLayoutHorizontal("##header");
                ImNodal::LayoutSpring();
                ImNodal::BeginLayoutGroup();
                    ImGui::TextUnformatted("Hello");
                ImNodal::EndLayoutGroup();
                ImNodal::LayoutSpring();
            ImNodal::EndLayoutHorizontal();
            ImNodal::BeginLayoutHorizontal("##body");
                if (ImNodal::BeginSlot(2)) { ImGui::Text("in"); ImNodal::EndSlot(); }
                ImNodal::LayoutSpring();
                if (ImNodal::BeginSlot(3)) { ImGui::Text("out"); ImNodal::EndSlot(); }
            ImNodal::EndLayoutHorizontal();
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
bool BeginNode(Id nodeId, ImNodalNodeFlags flags = ImNodalNodeFlags_None);
void EndNode();

void   SetNodePos    (ImVec2 pos /* canvas space */);   // current node, inside Begin/EndNode
void   SetNextNodePos(Id nodeId, ImVec2 pos);           // by id, anywhere
ImVec2 GetNodePos    (Id nodeId);                       // by id, anywhere
```

Behavior is set via `ImNodalNodeFlags_` (movable by default, opt-out with
`NotMovable`, hide the body with `NoBody`, show a hover-handle bar with
`HoverHandle`, etc.). Per-node knobs that are NOT binary toggles use
`Set*` calls between Begin/End (`SetNodeColor`, `SetNodeHitbox`,
`SetNodeBodyShape`), ImGui-style.

**Position is owned by the lib.** Push an initial / restored position via
`SetNextNodePos(id, canvasPos)` ONCE (typically at first frame or when
loading from disk), then let `BeginNode` / drag manage it. Read it back
via `GetNodePos(id)` for saving. Inside a `BeginNode/EndNode` scope, the
parameterless `SetNodePos(pos)` writes to the current node (mirrors
ImGui's `SetWindowPos`). Do NOT call either setter every frame — that
would defeat dragging by clobbering the lib-stored position.

```cpp
// Init pass (load from disk, or first frame defaults — outside any scope)
ImNodal::SetNextNodePos(nodeId, ImVec2(120.0f, 40.0f));

// Render pass (every frame)
if (ImNodal::BeginNode(nodeId)) {
    // ... content ...
    // ImNodal::SetNodePos(ImVec2(x, y));  // optional, current node, ImGui-style
    ImNodal::EndNode();
}

// Save pass
const ImVec2 currentPos = ImNodal::GetNodePos(nodeId);
```

### What ImNodal paints itself

Inside `EndNode`, ImNodal only paints :
- the **body fill** rectangle (color `ImNodalCol_NodeBody`, rounded corners
  with `ImNodalStyleVar_NodeRounding`).
- the **border** (color `ImNodalCol_NodeBorder`, or `NodeBorderSelected`
  when the node is selected; thickness from `ImNodalStyleVar_NodeBorderThickness`).
- an optional **hover-handle bar** at the top, when `ImNodalNodeFlags_HoverHandle`
  is set and the mouse is over the node (used by reroute primitives).

There is **no header tint, no body sectioning, no automatic centering**
done by ImNodal. The host owns all of that — typically by painting a
colored band into `GetNodeBackgroundDrawList(id)` over the upper part of
the node rect and assembling content with `BeginLayoutHorizontal/Vertical`
+ `LayoutSpring` + `BeginLayoutGroup`.

### Non-rect body — `SetNodeBodyShape`

Call `SetNodeBodyShape(const ImNodalHitbox&)` between `BeginNode` and
`EndNode` to make the lib paint the body as a **circle** or **convex
polygon** instead of the default rounded rect, using the same theme
colors (`ImNodalCol_NodeBody` fill, `ImNodalCol_NodeBorder` /
`NodeBorderSelected` outline) and channel ordering. `NodeRounding` is
ignored for non-rect shapes.

```cpp
static ImVec2 pts[4] = { top, right, bottom, left };
ImNodalHitbox shape;
shape.type = ImNodalHitShape_ConvexPolygon;
shape.polygonPoints = pts;
shape.polygonCount = 4;

if (ImNodal::BeginNode(nodeId)) {
    ImGui::Dummy(ImVec2(2*halfW, 2*halfH));   // reserve footprint
    ImNodal::SetNodeBodyShape(shape);          // lib paints the diamond
    ImNodal::SetNodeHitbox(shape);             // same shape → clickable visual
    ImNodal::EndNode();
}
```

The body shape is **independent** from the hitbox — pass the same shape
to both for a visually-shaped clickable node, or only `SetNodeBodyShape`
to keep the default AABB hit area (generous click target around a fine
visual). Has no effect when `ImNodalNodeFlags_NoBody` is set (the host
owns the draw in that case). Polygon points are caller-owned and must
outlive the call up to `EndNode` — same rule as hitboxes.

### Layout primitives — `BeginLayoutHorizontal/Vertical` + `LayoutSpring` + `BeginLayoutGroup`

The host assembles the node's content with horizontal and vertical
containers, plus `LayoutSpring()` to distribute the remaining space along
the container's main axis, plus `BeginLayoutGroup/EndLayoutGroup` to wrap
bare ImGui widgets so they count as layout children. Usable ONLY inside
a `BeginNode/EndNode` scope.

```cpp
bool BeginLayoutHorizontal(const char* id, const ImVec2& size = ImVec2(-1.0f, 0.0f));
void EndLayoutHorizontal();
bool BeginLayoutVertical(const char* id, const ImVec2& size = ImVec2(0.0f, -1.0f));
void EndLayoutVertical();
void LayoutSpring(float weight = 1.0f);
bool BeginLayoutGroup();
void EndLayoutGroup();
```

`size.x` / `size.y` semantics, per axis :
- `> 0`  : forced size in pixels.
- `== 0` : natural size = sum of non-Spring children measured at the
  previous frame. `LayoutSpring()` is a no-op (no gap to fill).
- `< 0`  : fill parent (parent container's target along the same axis,
  or the node body width/height when the container is at the top).

`LayoutSpring(weight)` claims the gap between the container's target and
the sum of non-Spring children sizes. Multi-Spring is supported : each
`LayoutSpring` takes its proportional share of the gap based on its weight
(the sum of weights is measured at the previous frame).

**Important** — auto-`SameLine` between siblings is triggered by
`BeginLayoutHorizontal/Vertical`, `LayoutSpring`, and `BeginLayoutGroup`.
Bare ImGui widgets (`ImGui::Dummy`, `ImGui::TextUnformatted`,
`ImGui::Button`, ...) do NOT trigger it. To put a bare widget on the same
row as its siblings inside `BeginLayoutHorizontal`, wrap it in
`BeginLayoutGroup/EndLayoutGroup`.

```cpp
// Header centered on the node width :
ImNodal::BeginLayoutHorizontal("##header");
    ImNodal::LayoutSpring();
    ImNodal::BeginLayoutGroup();
        ImGui::TextUnformatted("Node title");
    ImNodal::EndLayoutGroup();
    ImNodal::LayoutSpring();
ImNodal::EndLayoutHorizontal();

// Body : inputs left, outputs right, Spring in between :
ImNodal::BeginLayoutHorizontal("##body");
    if (BeginSlot(in_id))  { ImGui::Text("in");  EndSlot(); }
    ImNodal::LayoutSpring();
    if (BeginSlot(out_id)) { ImGui::Text("out"); EndSlot(); }
ImNodal::EndLayoutHorizontal();
```

Children of an horizontal container are auto-`SameLine`d when each child
is a Layout primitive (BeginLayout*, LayoutSpring, BeginLayoutGroup) — no
manual `SameLine` needed between them. Children of a vertical container
stack naturally.

**1-frame lag** : `LayoutSpring` uses the natural size measured at frame
N-1 to compute its fill at frame N. First frame falls back to "no gap" —
the node may be slightly off for one frame after a resize, then converges.

### Painting your own header tint

The recommended pattern : assemble the header with
`BeginLayoutHorizontal/LayoutSpring/BeginLayoutGroup/Text/EndLayoutGroup/LayoutSpring/EndLayoutHorizontal`,
read `ImGui::GetItemRectMin()` / `Max()` after `EndLayoutHorizontal`, then
in/after `EndNode` paint the band into the node background draw list.

```cpp
BeginNode(id);
    BeginLayoutHorizontal("##header");
        LayoutSpring();
        BeginLayoutGroup();
            ImGui::TextUnformatted(name);
        EndLayoutGroup();
        LayoutSpring();
    EndLayoutHorizontal();
    const ImVec2 headerMin = ImGui::GetItemRectMin();
    const ImVec2 headerMax = ImGui::GetItemRectMax();
    BeginLayoutHorizontal("##body");
        // ... slots ...
    EndLayoutHorizontal();
EndNode();

// Header band : full-width tint above the body fill.
const ImRect nodeRect = GetNodeRect(id);
if (auto* dl = GetNodeBackgroundDrawList(id)) {
    const float r = GetStyleVarFloat(ImNodalStyleVar_NodeRounding);
    dl->AddRectFilled(
        ImVec2(nodeRect.Min.x, nodeRect.Min.y),
        ImVec2(nodeRect.Max.x, headerMax.y),
        myHeaderColor, r, ImDrawFlags_RoundCornersTop);
}
```

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
bool BeginSlot(Id, ImNodalSlotFlags = ImNodalSlotFlags_None);
void EndSlot();
```

ImNodal does not impose any input/output/inout taxonomy on slots — every
slot is just a hit area + link anchor. The host encodes its own typing
(direction, data type, color ...) on its own side (typically an
`std::unordered_map<Id, MyType>` or fields on the host's slot object) and
applies its connection rules in `BeginConnectionCreate` / `QueryNewLink`
/ `AcceptNewLink`.

`BeginSlot` opens an ImGui group and `EndSlot` closes it. **ImNodal does
not render anything inside** — no dot, no label, no padding. The host owns
the visible appearance ; ImNodal owns the hit area, the link pivot and the
drag state machine.

### The recommended pattern

Reserve space, emit your widgets, end the slot, then paint your dot at the
slot pivot returned by `GetSlotScreenPos` :

```cpp
if (ImNodal::BeginSlot(slotId)) {
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

ImNodal does NOT paint any hover halo around the slot itself — the host
draws whatever feedback it wants, typically by reading
`GetSlotHitRect(id)` and `IsSlotHovered(id)` and emitting a translucent
`AddRectFilled` + `AddRect` over the slot's rect.

### Default link pivot and tangent

The **pivot** (point where links connect) defaults to the CENTER of the
group rect, same convention as thedmd/imgui-node-editor (`PivotAlignment
= (0.5, 0.5)`, `PivotSize = (0, 0)`). Push it to a different edge via
`SlotAlignment` (see below).

The **link tangent** (direction the link curve leaves the slot) is
derived from `slotAlignment`. The mapping :

| alignment | tangent |
|---|---|
| `x ≤ 0.25` | `(-1, 0)` |
| `x ≥ 0.75` | `( 1, 0)` |
| `y ≤ 0.25` | `(0, -1)` |
| `y ≥ 0.75` | `(0,  1)` |
| centered (default `(0.5, 0.5)`) | `(0, 0)` — resolved dynamically by `Link` |

Reroute / bridge slots leave the alignment at the centered default and
get the `(0, 0)` tangent that way.

The hit rect equals the group rect (no inflation, since there is no dot to
extend around).

### Empty-slot fallback

If the host emits nothing between `BeginSlot` and `EndSlot`, ImNodal
substitutes a `Dummy(Style.SlotMinSize)` so the slot still has a clickable
area :

```cpp
if (ImNodal::BeginSlot(id)) ImNodal::EndSlot();   // minimal button

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
if (BeginSlot(id)) {
    SlotAlignment(ImVec2(1.0f, 0.5f));
    ImGui::TextUnformatted("name");
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(12, 12));            // <- where you'll paint the dot
    EndSlot();
}

// Pivot OUTSIDE the group rect : extend the pivot rect past the right edge
// with SlotSize, the link endpoint sits at its center.
if (BeginSlot(id)) {
    SlotAlignment(ImVec2(1.0f, 0.5f));
    SlotSize     (ImVec2(16.0f, 0.0f));      // pivot rect = (right edge ... +16px)
    ImGui::TextUnformatted("name");
    EndSlot();
}
```

The link tangent is derived from `slotAlignment` (see the table above).

### Slot queries

```cpp
ImVec2 GetSlotScreenPos(Id);       // pivot, screen space
ImVec2 GetSlotTangent  (Id);       // unit vector pointing away from the slot
bool   IsSlotHovered   (Id);
bool   IsSlotConnected (Id);
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
const ImU32 col = ImNodal::GetStyleColorU32(ImNodalCol_SlotDot);
const float r   = ImNodal::GetStyleVarFloat(ImNodalStyleVar_SlotDotRadius);
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
one slot at the node center (centered `slotAlignment` → `(0, 0)` tangent).
Capture-only, like the slot :
ImNodal sets up the geometry, sizes the hit area to a circle of
`aHitRadius` around the slot pivot, and **the host paints the visible dot
+ selection ring AFTER `EndRerouteNode`**.

```cpp
bool BeginRerouteNode(Id nodeId, Id slotId, ImVec2* pos,
                      ImNodalNodeFlags flags = ImNodalNodeFlags_None,
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

## Automatic layouts (`ILayout`)

Pluggable graph-arrangement strategies live in a **separate header**,
`ImNodalLayouts.h` (it pulls in `<vector>`; the core `ImNodal.h` keeps its
pointer+count ABI untouched). A layout reads a library-neutral description of
the graph and writes the node positions back **in place** — the same
"pass a buffer, it gets filled" idiom ImGui uses everywhere.

### Data model

```cpp
struct LayoutNode {
    Id     id;     // host node id (echoed back so the caller can correlate)
    ImVec2 size;   // IN  : measured node size (drives column width / row packing)
    ImVec2 pos;    // IN  : current position / OUT : computed position
};
struct LayoutEdge {
    int  fromNode, toNode;   // IN : indices into LayoutGraph::nodes (provider -> consumer)
    int  fromSlot, toSlot;   // IN : slot height index on each side (crossing reduction)
    bool excluded;           // OUT : set when the edge is left out (cycle-closing back-edge)
};
struct LayoutGraph {         // in/out buffer
    std::vector<LayoutNode> nodes;
    std::vector<LayoutEdge> edges;   // reference nodes by index
};

class ILayout {
public:
    virtual ~ILayout() = default;
    virtual bool Apply(LayoutGraph&) = 0;   // reads size + edges, writes pos + excluded ; true if it placed a node
};
```

### Built-in — `HierarchicalLayout`

A one-shot static layered (Sugiyama-style) layout: back-edge removal (DFS),
ASAP layering, connected-component banding, barycenter ordering, then a
coordinate-assignment pass. Places nodes in left→right columns following the
link flow. Cycle-closing edges come back flagged via `LayoutEdge::excluded`
(draw them orthogonally). Tunable spacing:

```cpp
class HierarchicalLayout : public ILayout {
public:
    struct Settings {
        float columnGap  = 120.0f;  // horizontal gap between two layers
        float rowGap     = 35.0f;   // vertical gap between two nodes of a layer
        float clusterGap = 80.0f;   // extra gap between two connected components (bands)
    };
    HierarchicalLayout();
    explicit HierarchicalLayout(const Settings&);
    Settings& GetSettings();
    void      SetSettings(const Settings&);
    bool      Apply(LayoutGraph&) override;
};
```

### Two ways to run a layout

**1 — ImNodal drives it (recommended).** Call inside `BeginGraph/EndGraph`,
**after** every node + link has been emitted this frame (so node sizes and slot
owners are up to date — typically right before `EndGraph`). ImNodal collects the
topology from its own registry, runs the layout, and writes the new positions
back into its store **by id** (effective next frame, like `SetNextNodePos`).

```cpp
bool ApplyLayout(ILayout&);                      // raw form

template <class TLayout, class... TArgs>         // convenience : builds the layout for you
bool ApplyLayout(TArgs&&... args);

bool IsLinkExcludedByLayout(Id linkId);          // true for a back-edge from the last run
```

```cpp
// one-shot, e.g. behind an "Auto layout" menu item :
ImNodal::ApplyLayout<ImNodal::HierarchicalLayout>();             // defaults
ImNodal::ApplyLayout<ImNodal::HierarchicalLayout>(mySettings);  // tuned

// pick a routing per link from the back-edge classification :
for (auto& link : myLinks) {
    const bool ortho = ImNodal::IsLinkExcludedByLayout(link.id);
    // draw link.id as a Manhattan polyline if ortho, else as a spline
}
```

`ApplyLayout` is **one-shot** — call it on demand, not every frame.

**2 — Standalone.** Build a `LayoutGraph` yourself, run the algorithm, and push
the results wherever your nodes live. Use this when ImNodal does not own the
positions (your own model / solver does).

```cpp
ImNodal::LayoutGraph g;
g.nodes.push_back({ nodeIdA, sizeA, {} });
g.nodes.push_back({ nodeIdB, sizeB, {} });
g.edges.push_back({ 0, 1, 0, 0, false });        // node 0 -> node 1

ImNodal::HierarchicalLayout layout;
if (layout.Apply(g)) {
    for (auto& n : g.nodes) { myStore[n.id].pos = n.pos; }   // n.pos was filled
    for (auto& e : g.edges) { if (e.excluded) { markOrtho(e); } }
}
```

### Settings editor

```cpp
bool ShowHierarchicalLayoutSettingsEditor(HierarchicalLayout::Settings&);
```

Drop-in widget (one `DragFloat` per gap + a reset button), same idiom as
`ShowStyleVarsEditor`. Returns `true` if any field changed this frame.

### Writing your own layout

Derive `ILayout`, fill `pos` (and optionally `excluded`) on the passed
`LayoutGraph`, return `true` if you placed at least one node. It then works with
both `ApplyLayout(myLayout)` and the standalone form. The interface knows nothing
about ImNodal internals, so a layout is trivially unit-testable in isolation.

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

Color enum (`ImNodalCol_*`) covers the grid lines, node body / border /
hover-handle (the only things ImNodal paints on the node), recommended
slot-dot colors (read by hosts and the reroute primitive), link states
(idle / hover / selected / preview-accept / preview-reject), reroute ring
colors, box selection, and flow dot.

Var enum (`ImNodalStyleVar_*`) covers node rounding, border thickness,
body padding, hover-handle height, recommended slot-dot radius (read by
hosts / reroute), slot min size (Dummy size for empty slots), link
thickness, grid size and grid subdivisions.

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
