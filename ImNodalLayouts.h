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

// ImNodal layouts — pluggable graph-arrangement strategies.
//
// A layout reads a library-neutral description of a graph (nodes with their
// measured size + directed flow edges) and writes, IN PLACE on the same struct,
// a position per node plus a per-edge "excluded" flag for the edges it could not
// embed (typically cycle-closing back-edges the host may want to draw
// orthogonally). No separate result object : you pass a LayoutGraph buffer and
// the layout fills it — the same "pass a buffer, it gets filled" idiom ImGui
// uses everywhere.
//
// The interface is intentionally decoupled from any node-graph data model : it
// knows nothing about ImNodal's internal NodeState / LinkState, nor about any
// host structure. Two ways to use it :
//   1. Build a LayoutGraph yourself, call ILayout::Apply, read the node `pos`
//      (and edge `excluded`) back and push them wherever your nodes live.
//   2. Let ImNodal drive it for the current graph with ImNodal::ApplyLayout
//      (collects the topology from its own registry, runs the layout, writes the
//      node positions back into the store BY id). The templated overload
//      ImNodal::ApplyLayout<HierarchicalLayout>(args...) constructs the layout
//      for you — see below.
//
// This header pulls in <vector> on purpose : a derivable layout interface
// inherently exchanges dynamically sized arrays, and ImNodal is linked
// statically here, so the std::vector ABI in these structs is a non-issue.
// The core ImNodal.h keeps its ptr+count ABI untouched.

#include "ImNodal.h"  // ImNodal::Id + ImNodal::ApplyLayout(ILayout&) + the imgui include chain (ImVec2)

#include <utility>
#include <vector>

namespace ImNodal {

// A node as seen by a layout algorithm — library-neutral.
struct LayoutNode {
    Id id{0};       // host node id — ImNodal::ApplyLayout correlates the result back by this id
    ImVec2 size{};  // IN  : measured node size, canvas units (drives column width / row packing)
    ImVec2 pos{};   // IN  : current position (incremental layouts may read it) / OUT : computed position
};

// A directed flow edge : provider/output side -> consumer/input side.
struct LayoutEdge {
    int fromNode{0};       // IN : index into LayoutGraph::nodes (provider side)
    int toNode{0};         // IN : index into LayoutGraph::nodes (consumer side)
    int fromSlot{0};       // IN : output-slot height index on the provider (used for crossing reduction)
    int toSlot{0};         // IN : input-slot height index on the consumer (used for crossing reduction)
    bool excluded{false};  // OUT : set by the layout when the edge is left out (cycle-closing back-edge)
};

// In/out buffer handed to a layout. Edges reference nodes by their index in
// `nodes`, so the caller builds a compact node set (e.g. filtering hidden nodes
// out and remapping indices) before calling Apply. Apply reads `size` + edges,
// writes `pos` + edge `excluded` in place.
struct LayoutGraph {
    std::vector<LayoutNode> nodes;
    std::vector<LayoutEdge> edges;
};

// Strategy interface — derive to implement a graph-arrangement algorithm. Apply
// reads node sizes + edges and writes node positions + edge `excluded` flags in
// place on aGraph. Returns true if at least one node was placed.
class ILayout {
public:
    virtual ~ILayout() = default;
    virtual bool Apply(LayoutGraph& aGraph) = 0;
};

// Built-in : one-shot STATIC hierarchical (layered) layout. It places the nodes
// in left->right columns following the link flow (provider/output ->
// consumer/input). Pure geometry : it only writes back onto aGraph, owns no
// per-run state and steps no simulation. Cycle-closing edges (back-edges) are
// reported through LayoutEdge::excluded so the host can draw them orthogonally.
class HierarchicalLayout : public ILayout {
public:
    // Tunable spacing of the layered layout (canvas units).
    struct Settings {
        float columnGap{120.0f};   // horizontal gap between two layers (columns)
        float rowGap{35.0f};       // vertical gap between two nodes of the same layer
        float clusterGap{80.0f};   // extra vertical gap between two different clusters (bands)
        Settings() = default;
    };

    HierarchicalLayout() = default;
    explicit HierarchicalLayout(const Settings& arSettings) : m_settings(arSettings) {}

    // Read / write the spacing settings used by Apply. Push your values once,
    // then call Apply ; the same instance can be reused across runs.
    Settings& GetSettings() { return m_settings; }
    const Settings& GetSettings() const { return m_settings; }
    void SetSettings(const Settings& arSettings) { m_settings = arSettings; }

    bool Apply(LayoutGraph& aGraph) override;

private:
    Settings m_settings{};
};

// Templated convenience over ApplyLayout(ILayout&) (declared in ImNodal.h) :
// construct the layout in place from its ctor args and run it on the current
// graph. Lets the host write
//
//     ImNodal::ApplyLayout<ImNodal::HierarchicalLayout>();
//     ImNodal::ApplyLayout<ImNodal::HierarchicalLayout>(mySettings);
//
// instead of instantiating the layout itself. TArgs are forwarded to TLayout's
// constructor. TLayout must derive from ILayout.
template <class TLayout, class... TArgs>
inline bool ApplyLayout(TArgs&&... aArgs) {
    TLayout layout{std::forward<TArgs>(aArgs)...};  // braces : avoids the most-vexing-parse on an empty pack
    return ApplyLayout(static_cast<ILayout&>(layout));
}

}  // namespace ImNodal
