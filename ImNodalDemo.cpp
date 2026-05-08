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

// ImNodalDemo — auto-contained showcase, ImGui::ShowDemoWindow style.
// Doubles as live reference code for hosts integrating ImNodal.
//
// Layout : MenuBar (Style dropdown) + left side panel grouped by feature
// section + right canvas. Graph starts EMPTY ; the user clicks buttons in
// the left panel to add nodes / reroutes / wire them. Each section lets
// the user tweak the demo for that feature.

#include "ImNodal.h"

#include <cmath>
#include <utility>
#include <vector>

namespace ImNodal {
namespace {

// -----------------------------
// Demo state
// -----------------------------

enum class LinkStyle {
    HorizontalBezier = 0,  // legacy : ImNodal::Link()
    VerticalBezier,        // BeginLink + LinkBezierSegment with vertical tangents
    Manhattan,             // LinkPolyline (4 points, axis-aligned)
    Sinus,                 // LinkLineSegment chain shaped as sinwave
};

static const char* kLinkStyleLabels[] = {
    "Horizontal bezier (legacy)",
    "Vertical bezier (top-down)",
    "Manhattan (polyline)",
    "Sinus wave (custom)",
};

struct DemoSlot {
    Id id;
    const char* label;
    ImU32 color;
};
struct DemoNode {
    Id id;
    ImVec2 pos;
    const char* title;
    ImU32 headerCol;
    std::vector<DemoSlot> inputs;
    std::vector<DemoSlot> outputs;
    bool verticalIO;   // true → inputs on top edge, outputs on bottom edge
    bool hasBody;
    float bodyValue;
};
struct DemoReroute {
    Id nodeId;
    Id slotId;
    ImVec2 pos;
    ImU32 color;
};
struct DemoLink {
    Id id;
    Id fromSlot;
    Id toSlot;
    ImU32 color;
    bool flow;
    LinkStyle style;
};

struct DemoState {
    Id nextId{0xD0000001ull};
    std::vector<DemoNode> nodes;
    std::vector<DemoReroute> reroutes;
    std::vector<DemoLink> links;

    LinkStyle currentLinkStyle{LinkStyle::HorizontalBezier};
    bool showStyleColors{false};
    bool showStyleVars{false};

    // MiniMap toggle + tweakable settings.
    bool          miniMapEnabled{true};
    MiniMapSettings miniMap{};

    // QueryNewNodeFromSlot → popup state
    Id pendingNewFromSlot{0};
    ImVec2 pendingNewPos{};

    Id allocId() { return nextId++; }

    DemoSlot* findSlot(Id slotId) {
        for (auto& n : nodes) {
            for (auto& s : n.inputs)  if (s.id == slotId) return &s;
            for (auto& s : n.outputs) if (s.id == slotId) return &s;
        }
        return nullptr;
    }

    void clearAll() {
        nodes.clear();
        reroutes.clear();
        links.clear();
        pendingNewFromSlot = 0;
    }

    void addLink(Id from, Id to, ImU32 col = 0) {
        if (from == to) return;
        for (auto& l : links) {
            if ((l.fromSlot == from && l.toSlot == to) ||
                (l.fromSlot == to && l.toSlot == from)) return;
        }
        const SlotRole r1 = GetSlotRole(from);
        const SlotRole r2 = GetSlotRole(to);
        if (r1 == SlotRole_Input && r2 == SlotRole_Output) std::swap(from, to);
        // Per-input single-fan : drop any existing link arriving at `to`.
        for (auto it = links.begin(); it != links.end();) {
            if (it->toSlot == to) it = links.erase(it);
            else ++it;
        }
        DemoLink l;
        l.id = allocId();
        l.fromSlot = from;
        l.toSlot = to;
        DemoSlot* fs = findSlot(from);
        l.color = col ? col : (fs ? fs->color : IM_COL32(200, 200, 200, 255));
        l.flow = false;
        l.style = currentLinkStyle;
        links.push_back(l);
    }
};

// -----------------------------
// Slot rendering helper
// -----------------------------
// Capture the slot, push the pivot to the edge that matches the dot
// orientation (input → leading edge, output → trailing edge), emit padding
// + optional label, then paint the dot AT GetSlotScreenPos — host-side
// rendering pattern recommended for ImNodal.

inline void DemoEmitSlot(SlotRole role, const DemoSlot& s, bool vertical) {
    if (!BeginSlot(s.id, role)) return;
    if (vertical) {
        // Top edge for inputs, bottom edge for outputs.
        SlotAlignment(role == SlotRole_Output ? ImVec2(0.5f, 1.0f) : ImVec2(0.5f, 0.0f));
    } else {
        SlotAlignment(role == SlotRole_Output ? ImVec2(1.0f, 0.5f) : ImVec2(0.0f, 0.5f));
    }
    constexpr float r = 5.0f;  // demo-local dot radius (host owns the dot draw)
    const float pad = r * 2.0f + 4.0f;
    if (vertical) {
        // Compact label, vertical layout doesn't need pre-padding.
        if (s.label && s.label[0]) ImGui::TextUnformatted(s.label);
        else ImGui::Dummy(ImVec2(pad, pad));
    } else if (role == SlotRole_Output) {
        if (s.label && s.label[0]) ImGui::TextUnformatted(s.label);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Dummy(ImVec2(pad, 0.0f));
    } else {
        ImGui::Dummy(ImVec2(pad, 0.0f));
        if (s.label && s.label[0]) {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(s.label);
        }
    }
    EndSlot();
    const ImVec2 c = GetSlotScreenPos(s.id);
    const ImU32 col = IsSlotHovered(s.id) ? IM_COL32_WHITE : s.color;
    ImGui::GetWindowDrawList()->AddCircleFilled(c, r, col);
}

inline void DemoPaintDot(Id slotId, ImU32 restCol, float radius = 5.0f) {
    const ImVec2 c = GetSlotScreenPos(slotId);
    const ImU32 col = IsSlotHovered(slotId) ? IM_COL32_WHITE : restCol;
    ImGui::GetWindowDrawList()->AddCircleFilled(c, radius, col);
}

// -----------------------------
// Node factories (called by left-panel buttons)
// -----------------------------

inline ImVec2 DemoSpawnPos(const DemoState& st) {
    // Spawn at the visible canvas center, offset by node count so successive
    // adds don't fully overlap. GetCanvasViewRect returns canvas-space.
    ImVec2 base(0.0f, 0.0f);
    if (GetCurrentContext() != nullptr) {
        const ImRect view = GetCanvasViewRect();
        if (view.GetWidth() > 0.0f) {
            base = ImVec2((view.Min.x + view.Max.x) * 0.5f - 60.0f,
                          (view.Min.y + view.Max.y) * 0.5f - 30.0f);
        }
    }
    const float k = (float)st.nodes.size();
    return ImVec2(base.x + k * 24.0f, base.y + k * 24.0f);
}

inline DemoNode& DemoAddSourceNode(DemoState& st) {
    DemoNode n{};
    n.id = st.allocId();
    n.pos = DemoSpawnPos(st);
    n.title = "Source";
    n.headerCol = IM_COL32(60, 90, 130, 255);
    n.outputs.push_back({st.allocId(), "value", IM_COL32(220, 80, 80, 255)});
    n.outputs.push_back({st.allocId(), "alpha", IM_COL32(80, 200, 100, 255)});
    n.hasBody = true;
    n.bodyValue = 0.5f;
    n.verticalIO = false;
    st.nodes.push_back(n);
    return st.nodes.back();
}

inline DemoNode& DemoAddProcessNode(DemoState& st) {
    DemoNode n{};
    n.id = st.allocId();
    n.pos = DemoSpawnPos(st);
    n.title = "Process";
    n.headerCol = IM_COL32(120, 80, 130, 255);
    n.inputs.push_back({st.allocId(), "in A", IM_COL32(220, 80, 80, 255)});
    n.inputs.push_back({st.allocId(), "in B", IM_COL32(80, 200, 100, 255)});
    n.outputs.push_back({st.allocId(), "out", IM_COL32(80, 140, 240, 255)});
    n.hasBody = true;
    n.bodyValue = 0.25f;
    n.verticalIO = false;
    st.nodes.push_back(n);
    return st.nodes.back();
}

inline DemoNode& DemoAddOutputNode(DemoState& st) {
    DemoNode n{};
    n.id = st.allocId();
    n.pos = DemoSpawnPos(st);
    n.title = "Output";
    n.headerCol = IM_COL32(80, 120, 80, 255);
    n.inputs.push_back({st.allocId(), "color", IM_COL32(80, 140, 240, 255)});
    n.hasBody = false;
    n.bodyValue = 0.0f;
    n.verticalIO = false;
    st.nodes.push_back(n);
    return st.nodes.back();
}

inline DemoNode& DemoAddEmptyNode(DemoState& st) {
    DemoNode n{};
    n.id = st.allocId();
    n.pos = DemoSpawnPos(st);
    n.title = "Empty";
    n.headerCol = IM_COL32(80, 80, 80, 255);
    n.hasBody = false;
    n.verticalIO = false;
    st.nodes.push_back(n);
    return st.nodes.back();
}

inline DemoNode& DemoAddMultiSlotNode(DemoState& st) {
    DemoNode n{};
    n.id = st.allocId();
    n.pos = DemoSpawnPos(st);
    n.title = "Multi";
    n.headerCol = IM_COL32(140, 100, 60, 255);
    const ImU32 cs[4] = {
        IM_COL32(220, 80, 80, 255),
        IM_COL32(80, 200, 100, 255),
        IM_COL32(80, 140, 240, 255),
        IM_COL32(220, 200, 80, 255),
    };
    n.inputs.push_back({st.allocId(), "i1", cs[0]});
    n.inputs.push_back({st.allocId(), "i2", cs[1]});
    n.inputs.push_back({st.allocId(), "i3", cs[2]});
    n.inputs.push_back({st.allocId(), "i4", cs[3]});
    n.outputs.push_back({st.allocId(), "o1", cs[0]});
    n.outputs.push_back({st.allocId(), "o2", cs[1]});
    n.outputs.push_back({st.allocId(), "o3", cs[2]});
    n.outputs.push_back({st.allocId(), "o4", cs[3]});
    n.hasBody = false;
    n.verticalIO = false;
    st.nodes.push_back(n);
    return st.nodes.back();
}

inline DemoNode& DemoAddVerticalNode(DemoState& st) {
    DemoNode n{};
    n.id = st.allocId();
    n.pos = DemoSpawnPos(st);
    n.title = "Vertical";
    n.headerCol = IM_COL32(60, 110, 130, 255);
    n.inputs.push_back({st.allocId(), "in", IM_COL32(220, 80, 80, 255)});
    n.outputs.push_back({st.allocId(), "out", IM_COL32(80, 140, 240, 255)});
    n.hasBody = false;
    n.verticalIO = true;
    st.nodes.push_back(n);
    return st.nodes.back();
}

// -----------------------------
// Link rendering — dispatched by style
// -----------------------------

inline void DemoRenderLink(const DemoLink& l) {
    switch (l.style) {
        case LinkStyle::HorizontalBezier:
        default:
            // Legacy API. Internally a wrapper around BeginLink/LinkBezierSegment/EndLink.
            Link(l.id, l.fromSlot, l.toSlot, l.color, 3.0f);
            break;

        case LinkStyle::VerticalBezier: {
            if (BeginLink(l.id, l.fromSlot, l.toSlot, 3.0f, l.color)) {
                // Force vertical tangents : start goes DOWN, end goes UP.
                LinkBezierSegment(GetLinkFromPos(), GetLinkToPos(),
                                  ImVec2(0.0f, 1.0f), ImVec2(0.0f, -1.0f), 32);
                EndLink();
            }
            break;
        }

        case LinkStyle::Manhattan: {
            if (BeginLink(l.id, l.fromSlot, l.toSlot, 3.0f, l.color)) {
                const ImVec2 a = GetLinkFromPos();
                const ImVec2 b = GetLinkToPos();
                const ImVec2 m1((a.x + b.x) * 0.5f, a.y);
                const ImVec2 m2((a.x + b.x) * 0.5f, b.y);
                const ImVec2 pts[4] = { a, m1, m2, b };
                LinkPolyline(pts, 4);
                EndLink();
            }
            break;
        }

        case LinkStyle::Sinus: {
            if (BeginLink(l.id, l.fromSlot, l.toSlot, 3.0f, l.color)) {
                const ImVec2 a = GetLinkFromPos();
                const ImVec2 b = GetLinkToPos();
                ImVec2 prev = a;
                constexpr int kSeg = 30;
                constexpr float kAmp = 12.0f;
                constexpr float kCycles = 2.0f;
                for (int i = 1; i <= kSeg; ++i) {
                    const float t = (float)i / (float)kSeg;
                    ImVec2 p(a.x * (1.0f - t) + b.x * t,
                            a.y * (1.0f - t) + b.y * t);
                    p.y += std::sin(t * 6.2831853f * kCycles) * kAmp;
                    LinkLineSegment(prev, p);
                    prev = p;
                }
                EndLink();
            }
            break;
        }
    }
}

// -----------------------------
// Slot anatomy section (kept from legacy demo, compacted)
// -----------------------------

inline void DemoDrawSlotAnatomy() {
    ImGui::TextWrapped(
        "BeginSlot/EndSlot is capture-only : ImNodal does not draw anything inside the "
        "slot. The host emits its widgets between Begin and End, then paints its dot AT "
        "GetSlotScreenPos(slotId) AFTER EndSlot. Default pivot = group center ; call "
        "SlotAlignment() to push it to an edge.");
    ImGui::Spacing();

    static float gSlider = 0.5f;

    if (BeginInputSlot(0xDA001ull)) EndSlot();
    DemoPaintDot(0xDA001ull, IM_COL32(255, 120, 120, 255));
    ImGui::SameLine();
    ImGui::TextDisabled("// naked slot, default pivot = group center");

    if (BeginInputSlot(0xDA002ull)) {
        SlotAlignment(ImVec2(0.0f, 0.5f));
        ImGui::Dummy(ImVec2(14, 0));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextUnformatted("with label, pivot pushed to left edge");
        EndSlot();
    }
    DemoPaintDot(0xDA002ull, IM_COL32(120, 220, 120, 255));

    if (BeginInputSlot(0xDA003ull)) {
        SlotAlignment(ImVec2(0.0f, 0.5f));
        ImGui::Dummy(ImVec2(14, 0));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextUnformatted("scale");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::SliderFloat("##s", &gSlider, 0.0f, 1.0f);
        EndSlot();
    }
    DemoPaintDot(0xDA003ull, IM_COL32(120, 160, 240, 255));

    if (BeginOutputSlot(0xDA004ull)) {
        SlotAlignment(ImVec2(1.0f, 0.5f));
        SlotSize(ImVec2(16.0f, 0.0f));
        ImGui::TextUnformatted("pivot pushed past right edge");
        EndSlot();
    }
    DemoPaintDot(0xDA004ull, IM_COL32(220, 200, 80, 255));
}

// -----------------------------
// Canvas / graph rendering
// -----------------------------

inline void DemoRenderNode(DemoNode& n) {
    NodeSettings ns;
    if (!BeginNode(n.id, &n.pos, ns))
        return;
    SetNodeColor(n.headerCol);  // accent color, picked up by the minimap (and future features)

    ImVec2 headerMin, headerMax;
    if (n.verticalIO) {
        // Layout vertical : top inputs row, header, body, bottom outputs row.
        BeginLayoutVertical("##vroot");
            if (!n.inputs.empty()) {
                BeginLayoutHorizontal("##in_row");
                    for (auto& s : n.inputs) DemoEmitSlot(SlotRole_Input, s, true);
                EndLayoutHorizontal();
            }
            BeginLayoutHorizontal("##header_v");
                LayoutSpring();
                // BeginLayoutGroup wraps the bare ImGui widget so it counts
                // as a layout child (auto-SameLine via s_emitChildSameLineIfH).
                BeginLayoutGroup();
                    ImGui::TextUnformatted(n.title);
                EndLayoutGroup();
                LayoutSpring();
            EndLayoutHorizontal();
            headerMin = ImGui::GetItemRectMin();
            headerMax = ImGui::GetItemRectMax();
            if (n.hasBody) {
                BeginLayoutGroup();
                    ImGui::SetNextItemWidth(80);
                    ImGui::SliderFloat("##v", &n.bodyValue, 0.0f, 1.0f);
                EndLayoutGroup();
            }
            if (!n.outputs.empty()) {
                BeginLayoutHorizontal("##out_row");
                    for (auto& s : n.outputs) DemoEmitSlot(SlotRole_Output, s, true);
                EndLayoutHorizontal();
            }
        EndLayoutVertical();
    } else {
        BeginLayoutHorizontal("##header");
            LayoutSpring();
            BeginLayoutGroup();
                ImGui::TextUnformatted(n.title);
            EndLayoutGroup();
            LayoutSpring();
        EndLayoutHorizontal();
        headerMin = ImGui::GetItemRectMin();
        headerMax = ImGui::GetItemRectMax();

        BeginLayoutHorizontal("##body");
            if (!n.inputs.empty()) {
                BeginLayoutVertical("##in");
                    for (auto& s : n.inputs) DemoEmitSlot(SlotRole_Input, s, false);
                EndLayoutVertical();
            }
            LayoutSpring();
            if (n.hasBody) {
                BeginLayoutVertical("##center");
                    BeginLayoutGroup();
                        ImGui::SetNextItemWidth(80);
                        ImGui::SliderFloat("##v", &n.bodyValue, 0.0f, 1.0f);
                    EndLayoutGroup();
                EndLayoutVertical();
                LayoutSpring();
            }
            if (!n.outputs.empty()) {
                BeginLayoutVertical("##out");
                    for (auto& s : n.outputs) DemoEmitSlot(SlotRole_Output, s, false);
                EndLayoutVertical();
            }
        EndLayoutHorizontal();
    }
    EndNode();

    // Host-side header band tint.
    if (ImGui::IsItemVisible() && headerMax.y > headerMin.y) {
        const ImRect nodeRect = GetNodeRect(n.id);
        if (nodeRect.GetWidth() > 0.0f) {
            if (auto* bgList = GetNodeBackgroundDrawList(n.id)) {
                const float rounding = GetStyleVarFloat(ImNodalStyleVar_NodeRounding);
                bgList->AddRectFilled(
                    ImVec2(nodeRect.Min.x, nodeRect.Min.y),
                    ImVec2(nodeRect.Max.x, headerMax.y),
                    n.headerCol, rounding, ImDrawFlags_RoundCornersTop);
            }
        }
    }
}

inline void DemoDrawCanvas(DemoState& st) {
    CanvasSettings cs;
    if (!BeginCanvas("##imnodal_demo_canvas", ImVec2(0.0f, 0.0f), cs))
        return;

    GraphSettings gs;
    if (BeginGraph(0xDEADBEEFull, gs)) {
        for (auto& n : st.nodes) DemoRenderNode(n);

        // Reroutes — a body-less node + an InOut slot with a circular hitbox.
        // Host paints the visible dot + selection ring. ImNodal no longer
        // ships a "reroute" primitive; the same effect is achieved with the
        // generic flags + SetNodeHitbox / SetSlotHitbox APIs.
        for (auto& rr : st.reroutes) {
            constexpr float kRR = 5.0f;
            NodeSettings nset;
            nset.flags = ImNodalNodeFlags_NoBody | ImNodalNodeFlags_HiddenInMinimap;
            if (BeginNode(rr.nodeId, &rr.pos, nset)) {
                BeginSlot(rr.slotId, SlotRole_InOut);
                const ImVec2 d(kRR * 2.0f, kRR * 2.0f);
                ImGui::Dummy(d);
                const ImVec2 c = ImGui::GetItemRectMin() + d * 0.5f;
                // Slot hit = the dot itself; node hit = a slightly larger
                // ring so clicking just outside the dot drags the reroute
                // (UE-style: dot starts a wire, ring drags the node).
                ImNodalHitbox slotHit;
                slotHit.type = ImNodalHitShape_Circle;
                slotHit.center = c;
                slotHit.radius = kRR;
                SetSlotHitbox(slotHit);
                EndSlot();
                ImNodalHitbox nodeHit;
                nodeHit.type = ImNodalHitShape_Circle;
                nodeHit.center = c;
                nodeHit.radius = kRR + 6.0f;
                SetNodeHitbox(nodeHit);
                EndNode();
            }
            const ImVec2 c = GetSlotScreenPos(rr.slotId);
            auto* dl = ImGui::GetWindowDrawList();
            const ImU32 dotCol = IsSlotHovered(rr.slotId) ? IM_COL32_WHITE : rr.color;
            dl->AddCircleFilled(c, kRR, dotCol);
            if (IsNodeSelected(rr.nodeId)) {
                dl->AddCircle(c, kRR + 4.0f, IM_COL32(255, 220, 80, 255), 0, 1.5f);
            }
        }

        // Links (style-dispatched).
        for (auto& l : st.links) {
            DemoRenderLink(l);
            if (l.flow) FlowLink(l.id, 1.0f, 0);
        }

        // Connection create.
        if (BeginConnectionCreate()) {
            Id from = 0, to = 0;
            if (QueryNewLink(&from, &to)) {
                const SlotRole rf = GetSlotRole(from);
                const SlotRole rt = GetSlotRole(to);
                const bool ok = (rf != rt) || (rf == SlotRole_InOut) || (rt == SlotRole_InOut);
                if (ok) { if (AcceptNewLink()) st.addLink(from, to); }
                else    { RejectNewLink("input <-> input or output <-> output"); }
            } else if (QueryNewNodeFromSlot(&from)) {
                if (AcceptNewNodeFromSlot()) {
                    st.pendingNewFromSlot = from;
                    st.pendingNewPos = ScreenToCanvas(ImGui::GetIO().MousePos);
                    ImGui::OpenPopup("##imnodal_demo_create_node");
                }
            }
            EndConnectionCreate();
        }

        // Delete (Del key).
        if (BeginDelete()) {
            Id id = 0;
            while (QueryDeletedLink(&id)) {
                if (AcceptDelete()) {
                    for (auto it = st.links.begin(); it != st.links.end(); ++it) {
                        if (it->id == id) { st.links.erase(it); break; }
                    }
                }
            }
            while (QueryDeletedNode(&id)) {
                if (AcceptDelete()) {
                    auto eraseLinksOfNode = [&](Id nodeId) {
                        for (auto& n : st.nodes) {
                            if (n.id != nodeId) continue;
                            auto isOurs = [&](Id sid) {
                                for (auto& s : n.inputs)  if (s.id == sid) return true;
                                for (auto& s : n.outputs) if (s.id == sid) return true;
                                return false;
                            };
                            for (auto it = st.links.begin(); it != st.links.end();) {
                                if (isOurs(it->fromSlot) || isOurs(it->toSlot)) it = st.links.erase(it);
                                else ++it;
                            }
                        }
                    };
                    eraseLinksOfNode(id);
                    for (auto it = st.nodes.begin(); it != st.nodes.end(); ++it) {
                        if (it->id == id) { st.nodes.erase(it); break; }
                    }
                    for (auto it = st.reroutes.begin(); it != st.reroutes.end(); ++it) {
                        if (it->nodeId == id) {
                            const Id rs = it->slotId;
                            for (auto lit = st.links.begin(); lit != st.links.end();) {
                                if (lit->fromSlot == rs || lit->toSlot == rs) lit = st.links.erase(lit);
                                else ++lit;
                            }
                            st.reroutes.erase(it);
                            break;
                        }
                    }
                }
            }
            EndDelete();
        }

        // MiniMap on top of the graph (call AFTER nodes so their last-frame
        // screen rects are populated).
        if (st.miniMapEnabled) {
            ShowMiniMap(st.miniMap);
        }

        EndGraph();
    }
    EndCanvas();

    // Create-node popup, opened by QueryNewNodeFromSlot above.
    if (ImGui::BeginPopup("##imnodal_demo_create_node")) {
        ImGui::TextDisabled("Create node");
        ImGui::Separator();
        if (ImGui::MenuItem("Reroute here")) {
            DemoReroute rr;
            rr.nodeId = st.allocId();
            rr.slotId = st.allocId();
            rr.pos = st.pendingNewPos;
            rr.color = IM_COL32(220, 200, 80, 255);
            st.reroutes.push_back(rr);
            st.addLink(st.pendingNewFromSlot, rr.slotId);
            st.pendingNewFromSlot = 0;
        }
        if (ImGui::MenuItem("Cancel")) {
            st.pendingNewFromSlot = 0;
        }
        ImGui::EndPopup();
    }
}

// -----------------------------
// Left-panel sections
// -----------------------------

inline void DemoSectionNodes(DemoState& st) {
    ImGui::TextWrapped("Click to add nodes at the visible canvas center.");
    ImGui::Spacing();
    if (ImGui::Button("+ Source",   ImVec2(-FLT_MIN, 0.0f))) DemoAddSourceNode(st);
    if (ImGui::Button("+ Process",  ImVec2(-FLT_MIN, 0.0f))) DemoAddProcessNode(st);
    if (ImGui::Button("+ Output",   ImVec2(-FLT_MIN, 0.0f))) DemoAddOutputNode(st);
    ImGui::Separator();
    if (ImGui::Button("+ Empty",    ImVec2(-FLT_MIN, 0.0f))) DemoAddEmptyNode(st);
    if (ImGui::Button("+ Multi-slot (4 in / 4 out)", ImVec2(-FLT_MIN, 0.0f))) DemoAddMultiSlotNode(st);
    if (ImGui::Button("+ Vertical I/O (top → bottom)", ImVec2(-FLT_MIN, 0.0f))) DemoAddVerticalNode(st);
    ImGui::Separator();
    if (ImGui::Button("Add demo set + wire", ImVec2(-FLT_MIN, 0.0f))) {
        DemoNode& a = DemoAddSourceNode(st);
        DemoNode& b = DemoAddProcessNode(st);
        DemoNode& c = DemoAddOutputNode(st);
        if (!a.outputs.empty() && b.inputs.size() >= 1) st.addLink(a.outputs[0].id, b.inputs[0].id);
        if (a.outputs.size() >= 2 && b.inputs.size() >= 2) st.addLink(a.outputs[1].id, b.inputs[1].id);
        if (!b.outputs.empty() && !c.inputs.empty())     st.addLink(b.outputs[0].id, c.inputs[0].id);
    }
}

inline void DemoSectionLinks(DemoState& st) {
    ImGui::TextWrapped(
        "Style applied to NEW links created by drag-and-drop. The first 'Horizontal "
        "bezier' uses ImNodal::Link() ; the others are built with the agnostic primitives "
        "BeginLink + LinkBezierSegment / LinkPolyline / LinkLineSegment.");
    ImGui::Spacing();
    int idx = (int)st.currentLinkStyle;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##linkstyle", &idx, kLinkStyleLabels, IM_ARRAYSIZE(kLinkStyleLabels))) {
        st.currentLinkStyle = (LinkStyle)idx;
    }
    ImGui::Spacing();
    if (ImGui::Button("Apply style to ALL existing links", ImVec2(-FLT_MIN, 0.0f))) {
        for (auto& l : st.links) l.style = st.currentLinkStyle;
    }
    ImGui::TextDisabled("(Tip : Vertical bezier pairs nicely with the Vertical I/O node.)");
}

inline void DemoSectionReroutes(DemoState& st) {
    ImGui::TextWrapped(
        "Reroutes are zero-size nodes hosting a single InOut slot. They can be spliced "
        "anywhere on a path.");
    ImGui::Spacing();
    if (ImGui::Button("+ Reroute (visible center)", ImVec2(-FLT_MIN, 0.0f))) {
        DemoReroute rr;
        rr.nodeId = st.allocId();
        rr.slotId = st.allocId();
        ImVec2 pos(0.0f, 0.0f);
        if (GetCurrentContext() != nullptr) {
            const ImRect view = GetCanvasViewRect();
            if (view.GetWidth() > 0.0f)
                pos = ImVec2((view.Min.x + view.Max.x) * 0.5f,
                             (view.Min.y + view.Max.y) * 0.5f);
        }
        rr.pos = pos;
        rr.color = IM_COL32(220, 200, 80, 255);
        st.reroutes.push_back(rr);
    }
    ImGui::TextDisabled("Or drag a slot into empty canvas to open the 'Reroute here' popup.");
}

inline void DemoSectionFlow(DemoState& st) {
    ImGui::TextWrapped(
        "FlowLink animates dots along the link's path. Works on any link shape since dots "
        "step the cached polyline by arc-length.");
    ImGui::Spacing();
    if (ImGui::Button("Toggle on selected", ImVec2(-FLT_MIN, 0.0f))) {
        Id sel[64];
        const int n = GetSelectedLinks(sel, 64);
        for (int i = 0; i < n; ++i) {
            for (auto& l : st.links) {
                if (l.id == sel[i]) l.flow = !l.flow;
            }
        }
    }
    if (ImGui::Button("Toggle on ALL", ImVec2(-FLT_MIN, 0.0f))) {
        bool any = false;
        for (auto& l : st.links) if (l.flow) { any = true; break; }
        for (auto& l : st.links) l.flow = !any;
    }
    int flowing = 0;
    for (auto& l : st.links) if (l.flow) ++flowing;
    ImGui::Text("Flowing : %d / %d link(s)", flowing, (int)st.links.size());
}

inline void DemoSectionMiniMap(DemoState& st) {
    ImGui::Checkbox("Enabled", &st.miniMapEnabled);
    ImGui::TextDisabled("Click/drag in the map to recenter (UE-style).");
    ImGui::TextDisabled("Wheel inside the map to zoom.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat2("Size", &st.miniMap.size.x, 60.0f, 400.0f, "%.0f px");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat2("Offset", &st.miniMap.offset.x, 0.0f, 80.0f, "%.0f px");

    static const char* kAnchorLabels[] = {"Top-Left", "Top-Right", "Bottom-Left", "Bottom-Right"};
    int a = (int)st.miniMap.anchor;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("Anchor", &a, kAnchorLabels, IM_ARRAYSIZE(kAnchorLabels))) {
        st.miniMap.anchor = (ImNodalCorner)a;
    }

    // Background : transparency slider on the alpha channel of bgColor.
    ImVec4 bg = ImGui::ColorConvertU32ToFloat4(
        st.miniMap.bgColor != 0 ? st.miniMap.bgColor : GetStyleColorU32(ImNodalCol_MiniMapBg));
    if (ImGui::ColorEdit4("Bg color", &bg.x, ImGuiColorEditFlags_AlphaBar)) {
        st.miniMap.bgColor = ImGui::ColorConvertFloat4ToU32(bg);
    }

    ImVec4 br = ImGui::ColorConvertU32ToFloat4(
        st.miniMap.borderColor != 0 ? st.miniMap.borderColor : GetStyleColorU32(ImNodalCol_MiniMapBorder));
    if (ImGui::ColorEdit4("Border", &br.x, ImGuiColorEditFlags_AlphaBar)) {
        st.miniMap.borderColor = ImGui::ColorConvertFloat4ToU32(br);
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("Border thick.", &st.miniMap.borderThickness, 0.5f, 6.0f, "%.1f px");

    ImVec4 vp = ImGui::ColorConvertU32ToFloat4(
        st.miniMap.viewportRectColor != 0 ? st.miniMap.viewportRectColor : GetStyleColorU32(ImNodalCol_MiniMapViewport));
    if (ImGui::ColorEdit4("Viewport rect", &vp.x, ImGuiColorEditFlags_AlphaBar)) {
        st.miniMap.viewportRectColor = ImGui::ColorConvertFloat4ToU32(vp);
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("Viewport thick.", &st.miniMap.viewportRectThickness, 0.5f, 6.0f, "%.1f px");
}

inline void DemoSectionSelection(DemoState& /*st*/) {
    Id selN[64];
    Id selL[64];
    const int nN = GetSelectedNodes(selN, 64);
    const int nL = GetSelectedLinks(selL, 64);
    ImGui::Text("Selected : %d node(s), %d link(s)", nN, nL);
    const Id hN = GetHoveredNode();
    const Id hS = GetHoveredSlot();
    const Id hL = GetHoveredLink();
    ImGui::Text("Hovered  : node=0x%llx", (unsigned long long)hN);
    ImGui::Text("           slot=0x%llx", (unsigned long long)hS);
    ImGui::Text("           link=0x%llx", (unsigned long long)hL);
    ImGui::Spacing();
    ImGui::TextWrapped(
        "LMB-drag from canvas bg = box-select / Ctrl/Shift+click = multi-select / "
        "Del = delete selection / MMB-drag = pan / Wheel = zoom.");
}

inline void DemoSectionAnatomy() {
    DemoDrawSlotAnatomy();
}

inline void DemoSectionActions(DemoState& st) {
    if (ImGui::Button("Clear all", ImVec2(-FLT_MIN, 0.0f))) st.clearAll();
    ImGui::Spacing();
    ImGui::TextDisabled("Removes every node, reroute and link.");
}

}  // namespace

// =====================================================================
// Public entry point
// =====================================================================

IMNODAL_API void ShowDemoWindow(bool* apoOpen) {
    if (apoOpen != nullptr && !(*apoOpen)) return;

    ImGui::SetNextWindowSize(ImVec2(1100.0f, 720.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("ImNodal Demo", apoOpen, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }

    // The demo runs on its OWN ImNodal context so it can never pollute the
    // host's editor (panning the demo doesn't pan the host graph, right-clicks
    // don't bleed across, etc.). One Context = one editor.
    static Context* s_demoCtx = nullptr;
    if (s_demoCtx == nullptr) {
        s_demoCtx = CreateContext();
    }
    Context* const prevCtx = GetCurrentContext();
    SetCurrentContext(s_demoCtx);
    NewFrame();

    static DemoState s_demo;

    // ---- Menu bar ----
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Style")) {
            ImGui::MenuItem("Colors editor", nullptr, &s_demo.showStyleColors);
            ImGui::MenuItem("Vars editor",   nullptr, &s_demo.showStyleVars);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::TextDisabled("LMB drag = link slots / box-select empty canvas");
            ImGui::TextDisabled("MMB drag = pan canvas");
            ImGui::TextDisabled("Wheel    = zoom");
            ImGui::TextDisabled("Del      = delete selection");
            ImGui::TextDisabled("R        = reset zoom");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::Text("ImNodal %s", IMNODAL_VERSION);
    ImGui::SameLine();
    ImGui::TextDisabled("(graph starts empty — add nodes from the left panel)");
    ImGui::Separator();

    // ---- Two-pane layout : left side panel + canvas ----
    constexpr float kPanelW = 320.0f;
    ImGui::BeginChild("##imnodal_demo_panel", ImVec2(kPanelW, 0.0f),
                      ImGuiChildFlags_Borders);
    if (ImGui::CollapsingHeader("Nodes", ImGuiTreeNodeFlags_DefaultOpen))
        DemoSectionNodes(s_demo);
    if (ImGui::CollapsingHeader("Link style"))
        DemoSectionLinks(s_demo);
    if (ImGui::CollapsingHeader("Reroutes"))
        DemoSectionReroutes(s_demo);
    if (ImGui::CollapsingHeader("Flow animation"))
        DemoSectionFlow(s_demo);
    if (ImGui::CollapsingHeader("MiniMap"))
        DemoSectionMiniMap(s_demo);
    if (ImGui::CollapsingHeader("Selection / hover"))
        DemoSectionSelection(s_demo);
    if (ImGui::CollapsingHeader("Slot anatomy"))
        DemoSectionAnatomy();
    if (ImGui::CollapsingHeader("Actions"))
        DemoSectionActions(s_demo);
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##imnodal_demo_canvas_pane", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders);
    DemoDrawCanvas(s_demo);
    ImGui::EndChild();

    ImGui::End();

    // ---- Floating windows toggled from the Style menu ----
    // Still on the demo context so the editors edit the demo's style.
    if (s_demo.showStyleColors) {
        ImGui::SetNextWindowSize(ImVec2(420.0f, 600.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("ImNodal — Style Colors", &s_demo.showStyleColors)) {
            ShowStyleColorsEditor();
        }
        ImGui::End();
    }
    if (s_demo.showStyleVars) {
        ImGui::SetNextWindowSize(ImVec2(420.0f, 500.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("ImNodal — Style Vars", &s_demo.showStyleVars)) {
            ShowStyleVarsEditor();
        }
        ImGui::End();
    }

    // Restore the host's context so subsequent ImNodal calls (the host's
    // own editor) target the right Context.
    SetCurrentContext(prevCtx);
}

}  // namespace ImNodal
