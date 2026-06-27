# ImNodal

Zoomable / pannable **node-graph** primitives for [Dear ImGui](https://github.com/ocornut/imgui).

ImNodal gives you a pannable, zoomable canvas and the building blocks of a node
editor — nodes, slots, links, selection, the connection-create / delete /
copy-paste state machines, a minimap, and pluggable automatic layouts — while
staying out of your way: **the slot is the primitive, and you paint your own
visuals.**

## Highlights

- **Zoom / pan canvas** where ImGui widgets scale with the zoom (local-space transform).
- **Capture-only slots** — ImNodal hit-tests and hands you the link pivot; you draw the dot.
- **Links** — cubic-bezier by default, or your own polylines (Manhattan / orthogonal).
- **Interaction state machines** — connection-create, delete, shortcuts (Ctrl+C/V/X/D/A), box-select.
- **Automatic layouts** — a pluggable `ILayout` interface + a built-in hierarchical (layered) layout.
- **MiniMap**, custom node / slot shapes & hitboxes, ImGui-style Push/Pop theming.
- Single-context model, ABI-checked, no dependency beyond ImGui.

## Quick API — a simple graph

```cpp
#include <ImNodal.h>

// --- startup (once) ---
ImNodal::SetCurrentContext(ImNodal::CreateContext());

// --- each frame ---
ImNodal::NewFrame();
if (ImNodal::BeginCanvas("canvas")) {
    if (ImNodal::BeginGraph(1)) {
        static bool s_init = true;
        if (s_init) {  // initial positions — ImNodal owns them afterwards (drag is native)
            ImNodal::SetNextNodePos(1, ImVec2(40, 40));
            ImNodal::SetNextNodePos(2, ImVec2(260, 90));
            s_init = false;
        }
        if (ImNodal::BeginNode(1)) {
            ImGui::TextUnformatted("Node A");
            if (ImNodal::BeginSlot(12)) { ImGui::TextUnformatted("out"); ImNodal::EndSlot(); }
            ImNodal::EndNode();
        }
        if (ImNodal::BeginNode(2)) {
            ImGui::TextUnformatted("Node B");
            if (ImNodal::BeginSlot(21)) { ImGui::TextUnformatted("in"); ImNodal::EndSlot(); }
            ImNodal::EndNode();
        }
        ImNodal::Link(100, 12, 21);   // wire A.out (slot 12) -> B.in (slot 21)
        ImNodal::EndGraph();
    }
    ImNodal::EndCanvas();
}

// --- shutdown (once) ---
ImNodal::DestroyContext();
```

That is a working, draggable two-node graph with one link. Ids are non-zero and
unique within the graph; nodes, slots and links share the id space.

## Auto-arrange the graph

Drop the manual `SetNextNodePos` and let the built-in layered layout place the
nodes — call it once (e.g. behind an "Auto layout" menu item), inside the graph
scope after the nodes + links are emitted:

```cpp
ImNodal::ApplyLayout<ImNodal::HierarchicalLayout>();   // left -> right, follows the link flow
```

## Documentation

- **[Documentation.md](Documentation.md)** — full developer reference (canvas, nodes,
  slots, links, state machines, layouts, style).
- `ImNodal::ShowDemoWindow()` — a runnable showcase of every feature; source in
  `ImNodalDemo.cpp`, copy/paste-friendly.

## License

MIT — © 2025-2026 Stephane Cuillerdier (aka Aiekick).
