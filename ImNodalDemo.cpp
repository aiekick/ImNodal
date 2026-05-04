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

#include "ImNodal.h"

#include <utility>
#include <vector>

namespace ImNodal {
namespace {

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
    float bodyValue;  // optional widget in the center (slider)
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
};
struct DemoState {
    bool initialized{false};
    Id nextId{0xD0000001ull};
    std::vector<DemoNode> nodes;
    std::vector<DemoReroute> reroutes;
    std::vector<DemoLink> links;

    // Connection-create UI : when the user drops a drag on empty canvas we
    // remember the source slot and open a "create node" popup next frame.
    Id pendingNewFromSlot{0};
    ImVec2 pendingNewPos{};

    Id allocId() { return nextId++; }

    DemoSlot& findSlot(Id slotId) {
        static DemoSlot dummy{0, "", 0};
        for (auto& n : nodes) {
            for (auto& s : n.inputs)  if (s.id == slotId) return s;
            for (auto& s : n.outputs) if (s.id == slotId) return s;
        }
        return dummy;
    }

    void addLink(Id from, Id to, ImU32 col = 0) {
        // No duplicates, no self-link.
        if (from == to) return;
        for (auto& l : links) {
            if ((l.fromSlot == from && l.toSlot == to) ||
                (l.fromSlot == to && l.toSlot == from)) return;
        }
        // Always store as output -> input. Look up roles.
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
        l.color = col ? col : findSlot(from).color;
        l.flow = false;
        links.push_back(l);
    }

    void init() {
        if (initialized) return;
        initialized = true;
        // Three demo nodes wired in series : Source -> Process -> Output.
        const ImU32 cR = IM_COL32(220, 80, 80, 255);
        const ImU32 cG = IM_COL32(80, 200, 100, 255);
        const ImU32 cB = IM_COL32(80, 140, 240, 255);
        const ImU32 cY = IM_COL32(220, 200, 80, 255);

        DemoNode src{};
        src.id = allocId();
        src.pos = ImVec2(40, 60);
        src.title = "Source";
        src.headerCol = IM_COL32(60, 90, 130, 255);
        src.outputs.push_back({allocId(), "value", cR});
        src.outputs.push_back({allocId(), "alpha", cG});
        src.bodyValue = 0.5f;
        nodes.push_back(src);

        DemoNode proc{};
        proc.id = allocId();
        proc.pos = ImVec2(280, 40);
        proc.title = "Process";
        proc.headerCol = IM_COL32(120, 80, 130, 255);
        proc.inputs.push_back({allocId(), "in A", cR});
        proc.inputs.push_back({allocId(), "in B", cG});
        proc.outputs.push_back({allocId(), "out", cB});
        proc.bodyValue = 0.25f;
        nodes.push_back(proc);

        DemoNode out{};
        out.id = allocId();
        out.pos = ImVec2(560, 80);
        out.title = "Output";
        out.headerCol = IM_COL32(80, 120, 80, 255);
        out.inputs.push_back({allocId(), "color", cB});
        nodes.push_back(out);

        // Initial wiring.
        addLink(nodes[0].outputs[0].id, nodes[1].inputs[0].id, cR);
        addLink(nodes[0].outputs[1].id, nodes[1].inputs[1].id, cG);
        addLink(nodes[1].outputs[0].id, nodes[2].inputs[0].id, cB);
        // Mark the last link as a flowing one for the demo.
        if (!links.empty()) links.back().flow = true;

        // A reroute spliced on the proc->output link, just for the show.
        DemoReroute rr;
        rr.nodeId = allocId();
        rr.slotId = allocId();
        rr.pos = ImVec2(450, 200);
        rr.color = cY;
        reroutes.push_back(rr);
    }
};

// Helper : capture the slot, push the pivot to the edge that matches our
// dot position (input → left, output → right), emit padding + optional
// label, then paint the dot AT GetSlotScreenPos — the host-side rendering
// pattern recommended for ImNodal.
inline void DemoEmitSlot(SlotRole role, const DemoSlot& s) {
    if (!BeginSlot(s.id, role)) return;
    SlotAlignment(role == SlotRole_Output ? ImVec2(1.0f, 0.5f) : ImVec2(0.0f, 0.5f));
    const float r = GetStyleVarFloat(ImNodalStyleVar_SlotDotRadius);
    const float pad = r * 2.0f + 4.0f;
    if (role == SlotRole_Output) {
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

// Helper : paint a circle at the slot pivot. Used by the anatomy demo to
// draw each example's dot AFTER its EndSlot call.
inline void DemoPaintDot(Id slotId, ImU32 restCol, float radius = 5.0f) {
    const ImVec2 c = GetSlotScreenPos(slotId);
    const ImU32 col = IsSlotHovered(slotId) ? IM_COL32_WHITE : restCol;
    ImGui::GetWindowDrawList()->AddCircleFilled(c, radius, col);
}

inline void DemoDrawSlotAnatomy() {
    ImGui::TextWrapped(
        "BeginSlot/EndSlot is capture-only : ImNodal does not draw anything inside the "
        "slot. The host emits its widgets between Begin and End, then paints its dot/icon "
        "AT GetSlotScreenPos(slotId) AFTER EndSlot. The default pivot is the CENTER of "
        "the group rect (thedmd convention) — call SlotAlignment() to push it to an edge.");
    ImGui::Spacing();

    static float gSlider = 0.5f;

    // Naked slot — host emits nothing, the SlotMinSize Dummy fallback kicks
    // in. Default pivot (group center) lands on the dummy center.
    if (BeginInputSlot(0xDA001ull)) EndSlot();
    DemoPaintDot(0xDA001ull, IM_COL32(255, 120, 120, 255));
    ImGui::SameLine();
    ImGui::TextDisabled("// naked slot, default pivot = group center");

    // Padded label slot. We push the pivot to the LEFT edge so the dot
    // we paint sits at the leading dummy, not in the middle of the label.
    if (BeginInputSlot(0xDA002ull)) {
        SlotAlignment(ImVec2(0.0f, 0.5f));
        ImGui::Dummy(ImVec2(14, 0));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextUnformatted("with label, pivot pushed to left edge");
        EndSlot();
    }
    DemoPaintDot(0xDA002ull, IM_COL32(120, 220, 120, 255));

    // Label + inline widget — pivot at the left edge again.
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

    // Output with SlotSize : the pivot rect extends 8px past the right edge
    // so the dot sits visibly OUTSIDE the group rect, ImGui-button style.
    if (BeginOutputSlot(0xDA004ull)) {
        SlotAlignment(ImVec2(1.0f, 0.5f));
        SlotSize(ImVec2(16.0f, 0.0f));
        ImGui::TextUnformatted("pivot pushed past right edge");
        EndSlot();
    }
    DemoPaintDot(0xDA004ull, IM_COL32(220, 200, 80, 255));
}

inline void DemoDrawGraph(DemoState& st) {
    CanvasSettings cs;  // defaults are fine
    // Canvas takes the remaining vertical space minus a margin for the tip
    // text and the bottom collapsing headers (~80 px). Fallback to 250 px
    // if the host window is too short to compute a sensible value.
    const float availY = ImGui::GetContentRegionAvail().y;
    const float canvasH = (availY > 200.0f) ? (availY - 80.0f) : 250.0f;
    if (!BeginCanvas("##imnodal_demo_canvas", ImVec2(0.0f, canvasH), cs)) return;

    GraphSettings gs;
    if (BeginGraph(0xDEADBEEFull, gs)) {
        // -- Nodes --
        // Layout assemble manuellement avec BeginH/BeginV/Spring : header
        // centre via Spring/Text/Spring, body en H avec inputs|center|outputs
        // chacun en V, Springs entre eux pour distribuer l'espace.
        for (auto& n : st.nodes) {
            NodeSettings ns;
            if (BeginNode(n.id, &n.pos, ns)) {
                ImVec2 headerMin, headerMax;
                BeginH("##header");
                    Spring();
                    ImGui::TextUnformatted(n.title);
                    Spring();
                EndH();
                headerMin = ImGui::GetItemRectMin();
                headerMax = ImGui::GetItemRectMax();

                BeginH("##body");
                    if (!n.inputs.empty()) {
                        BeginV("##in");
                            for (auto& s : n.inputs) DemoEmitSlot(SlotRole_Input, s);
                        EndV();
                    }
                    Spring();
                    BeginV("##center");
                        ImGui::SetNextItemWidth(80);
                        ImGui::SliderFloat("##v", &n.bodyValue, 0.0f, 1.0f);
                    EndV();
                    Spring();
                    if (!n.outputs.empty()) {
                        BeginV("##out");
                            for (auto& s : n.outputs) DemoEmitSlot(SlotRole_Output, s);
                        EndV();
                    }
                EndH();
                EndNode();

                // Host-side header band tint : ImNodal n'a plus de header tint.
                // On peint un AddRectFilled coloré sur la bande supérieure du
                // node via le background draw list, en utilisant le node rect
                // (last frame) pour la largeur et headerMax.y pour la hauteur.
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
        }
        // -- Reroutes --
        // BeginRerouteNode is capture-only too : we paint dot + ring ourselves.
        for (auto& rr : st.reroutes) {
            constexpr float kRR = 5.0f;
            BeginRerouteNode(rr.nodeId, rr.slotId, &rr.pos, NodeSettings{}, kRR);
            EndRerouteNode();
            const ImVec2 c = GetSlotScreenPos(rr.slotId);
            auto* dl = ImGui::GetWindowDrawList();
            const ImU32 dotCol = IsSlotHovered(rr.slotId) ? IM_COL32_WHITE : rr.color;
            dl->AddCircleFilled(c, kRR, dotCol);
            if (IsNodeSelected(rr.nodeId)) {
                dl->AddCircle(c, kRR + 4.0f, IM_COL32(255, 220, 80, 255), 0, 1.5f);
            }
        }
        // -- Links --
        for (auto& l : st.links) {
            Link(l.id, l.fromSlot, l.toSlot, l.color, 3.0f);
            if (l.flow) FlowLink(l.id, 1.0f, 0);
        }

        // -- Connection create --
        if (BeginConnectionCreate()) {
            Id from, to = 0;
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

        // -- Delete (Del key) --
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

        EndGraph();
    }
    EndCanvas();

    // -- Create-node popup, opened from QueryNewNodeFromSlot above --
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

}  // namespace

IMNODAL_API void ShowDemoWindow(bool* apoOpen) {
    if (apoOpen != nullptr && !(*apoOpen)) return;
    ImGui::SetNextWindowSize(ImVec2(820.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("ImNodal Demo", apoOpen)) {
        ImGui::End();
        return;
    }
    if (GetCurrentContext() == nullptr) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "No ImNodal context — create one with CreateContext / SetCurrentContext.");
        ImGui::End();
        return;
    }

    static DemoState s_demo;
    s_demo.init();

    ImGui::Text("ImNodal %s", IMNODAL_VERSION);
    ImGui::SameLine();
    ImGui::TextDisabled(
        "(LMB drag = pan slot link / box-select | MMB drag = pan canvas | Wheel = zoom | "
        "Del = delete selection | R = reset zoom)");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Slot anatomy")) {
        DemoDrawSlotAnatomy();
    }
    if (ImGui::CollapsingHeader("Live graph", ImGuiTreeNodeFlags_DefaultOpen)) {
        DemoDrawGraph(s_demo);
        ImGui::TextDisabled(
            "Try : drag a slot to another slot to link / drag to empty canvas to "
            "open the create-node popup / select + Del / drag the reroute / wheel-zoom.");
    }
    if (ImGui::CollapsingHeader("Style - colors")) {
        ShowStyleColorsEditor();
    }
    if (ImGui::CollapsingHeader("Style - vars")) {
        ShowStyleVarsEditor();
    }
    ImGui::End();
}

}  // namespace ImNodal
