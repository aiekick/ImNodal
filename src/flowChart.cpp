#include "flowChart.h"

#include <imgui.h>
#include <cmath>
#include <vector>

// Only pull the Id type into our scope — a full `using namespace ImNodal;`
// would also pull ImNodal::FlowLink (the lib's flow-animation function),
// which collides with our FlowLink struct below.
using ImNodal::Id;

namespace {

// =====================================================================
// Data model — flat, no class, no helpers per kind.
// =====================================================================
enum FlowNodeKind {
    FlowNodeKind_Start = 0,
    FlowNodeKind_End,
    FlowNodeKind_Process,
    FlowNodeKind_Decision,
    FlowNodeKind_IO,
    FlowNodeKind_PredefinedProcess,
    FlowNodeKind_COUNT,
};

struct FlowNode {
    Id           id;
    FlowNodeKind kind;
    const char*  label;
};

struct FlowLink {
    Id id;
    Id from;
    Id to;
};

constexpr const char* k_kindNames[FlowNodeKind_COUNT] = {
    "Start",
    "End",
    "Process",
    "Decision",
    "Input / Output",
    "Predefined process",
};

// Slot id encoding : nodeId * 100 + slot index (1..3).
//   1 = input  (top)
//   2 = output 1 (bottom, or "Yes" for Decision)
//   3 = output 2 ("No" for Decision)
inline Id slotInput  (Id aNodeId) { return aNodeId * 100 + 1; }
inline Id slotOutput (Id aNodeId) { return aNodeId * 100 + 2; }
inline Id slotOutput2(Id aNodeId) { return aNodeId * 100 + 3; }

// Paint a small filled arrow head at `aTip` pointing along `aDir`.
inline void s_paintArrow(ImDrawList* apDL, ImVec2 aTip, ImVec2 aDir, ImU32 aCol) {
    const float len = std::sqrt(aDir.x * aDir.x + aDir.y * aDir.y);
    if (len < 0.001f) return;
    const ImVec2 n(aDir.x / len, aDir.y / len);
    const ImVec2 perp(-n.y, n.x);
    const float size = 9.0f;
    const ImVec2 base(aTip.x - n.x * size, aTip.y - n.y * size);
    const ImVec2 p1(base.x + perp.x * size * 0.5f, base.y + perp.y * size * 0.5f);
    const ImVec2 p2(base.x - perp.x * size * 0.5f, base.y - perp.y * size * 0.5f);
    ImVec2 pts[3] = { aTip, p1, p2 };
    apDL->AddConvexPolyFilled(pts, 3, aCol);
}

// Emit a slot of fixed footprint at the current cursor, with the given
// pivot alignment so the link anchor lands on the desired edge. Paints a
// small dot at the slot pivot afterwards (still inside the node scope so
// the dot lands on the right draw channel) — color reflects hover /
// connected state.
inline void s_emitSlot(Id aSlotId, ImVec2 aAlignment) {
    if (ImNodal::BeginSlot(aSlotId)) {
        ImNodal::SlotAlignment(aAlignment);
        ImGui::Dummy(ImVec2(10.0f, 10.0f));
        ImNodal::EndSlot();
        const ImVec2 c   = ImNodal::GetSlotScreenPos(aSlotId);
        const ImU32  col = ImNodal::IsSlotHovered(aSlotId)
            ? IM_COL32(255, 220, 120, 255)
            : (ImNodal::IsSlotConnected(aSlotId)
                ? IM_COL32(120, 200, 255, 255)
                : IM_COL32(180, 180, 190, 220));
        ImGui::GetWindowDrawList()->AddCircleFilled(c, 4.0f, col);
    }
}

} // anonymous namespace


void FlowChart::display() {
    // -----------------------------
    // Persistent data — all static, sits next to the function body.
    // -----------------------------
    static std::vector<FlowNode> s_nodes;
    static std::vector<FlowLink> s_links;
    static Id   s_nextNodeId = 100;
    static Id   s_nextLinkId = 5000;
    static bool s_inited     = false;

    // Popup staging — set by the canvas/graph logic, consumed by the
    // ImGui popups emitted outside the canvas scope.
    static bool   s_openCanvasPopup   = false;
    static ImVec2 s_canvasPopupPos    = ImVec2(0.0f, 0.0f);
    static bool   s_openDropPopup     = false;
    static ImVec2 s_dropPopupPos      = ImVec2(0.0f, 0.0f);
    static Id     s_dropFromSlot      = 0;

    // -----------------------------
    // First-frame seed — a tiny but representative flowchart.
    // -----------------------------
    if (!s_inited) {
        s_nodes.push_back({1, FlowNodeKind_Start,             "Start"});
        s_nodes.push_back({2, FlowNodeKind_IO,                "Read input"});
        s_nodes.push_back({3, FlowNodeKind_Decision,          "Valid?"});
        s_nodes.push_back({4, FlowNodeKind_Process,           "Report error"});
        s_nodes.push_back({5, FlowNodeKind_PredefinedProcess, "Compute"});
        s_nodes.push_back({6, FlowNodeKind_End,               "End"});
        s_nextNodeId = 7;

        ImNodal::SetNextNodePos(1, ImVec2(   0.0f, -260.0f));
        ImNodal::SetNextNodePos(2, ImVec2(   0.0f, -140.0f));
        ImNodal::SetNextNodePos(3, ImVec2(   0.0f,  -20.0f));
        ImNodal::SetNextNodePos(4, ImVec2(-200.0f,  120.0f));
        ImNodal::SetNextNodePos(5, ImVec2( 200.0f,  120.0f));
        ImNodal::SetNextNodePos(6, ImVec2(   0.0f,  260.0f));

        s_links.push_back({s_nextLinkId++, slotOutput (1), slotInput(2)});
        s_links.push_back({s_nextLinkId++, slotOutput (2), slotInput(3)});
        s_links.push_back({s_nextLinkId++, slotOutput2(3), slotInput(4)}); // "No"
        s_links.push_back({s_nextLinkId++, slotOutput (3), slotInput(5)}); // "Yes"
        s_links.push_back({s_nextLinkId++, slotOutput (4), slotInput(6)});
        s_links.push_back({s_nextLinkId++, slotOutput (5), slotInput(6)});
        s_inited = true;
    }

    // -----------------------------
    // Canvas + graph scope.
    // -----------------------------
    if (!ImNodal::BeginCanvas("##flowchart_canvas", ImVec2(0.0f, 0.0f))) {
        return;
    }
    if (ImNodal::BeginGraph(1)) {

        const float bodyW    = 140.0f;
        const float bodyH    =  60.0f;
        const ImU32 labelCol = IM_COL32(225, 230, 240, 255);

        // ---------------------------
        // Draw nodes — switch on kind, inline. No sub-function explosion.
        // ---------------------------
        for (const FlowNode& node : s_nodes) {
            switch (node.kind) {

                case FlowNodeKind_Process:
                case FlowNodeKind_PredefinedProcess: {
                    if (ImNodal::BeginNode(node.id)) {
                        s_emitSlot(slotInput(node.id), ImVec2(0.5f, 0.0f));
                        ImGui::Dummy(ImVec2(bodyW, bodyH - 20.0f));
                        const ImVec2 bMin = ImGui::GetItemRectMin();
                        const ImVec2 bMax = ImGui::GetItemRectMax();
                        const ImVec2 ts   = ImGui::CalcTextSize(node.label);
                        ImGui::GetWindowDrawList()->AddText(
                            ImVec2(bMin.x + (bMax.x - bMin.x - ts.x) * 0.5f,
                                   bMin.y + (bMax.y - bMin.y - ts.y) * 0.5f),
                            labelCol, node.label);
                        if (node.kind == FlowNodeKind_PredefinedProcess) {
                            // Two vertical bars inset from the rect edges.
                            ImDrawList* pDL = ImGui::GetWindowDrawList();
                            const ImU32 col = ImNodal::GetStyleColorU32(ImNodalCol_NodeBorder);
                            const float inset = 10.0f;
                            pDL->AddLine(ImVec2(bMin.x + inset, bMin.y), ImVec2(bMin.x + inset, bMax.y), col, 1.5f);
                            pDL->AddLine(ImVec2(bMax.x - inset, bMin.y), ImVec2(bMax.x - inset, bMax.y), col, 1.5f);
                        }
                        s_emitSlot(slotOutput(node.id), ImVec2(0.5f, 1.0f));
                        ImNodal::EndNode();
                    }
                    break;
                }

                case FlowNodeKind_Start:
                case FlowNodeKind_End: {
                    // Pill = rect with NodeRounding == bodyHeight / 2.
                    ImNodal::PushStyleVar(ImNodalStyleVar_NodeRounding, bodyH * 0.5f);
                    if (ImNodal::BeginNode(node.id)) {
                        if (node.kind == FlowNodeKind_End) {
                            s_emitSlot(slotInput(node.id), ImVec2(0.5f, 0.0f));
                        }
                        ImGui::Dummy(ImVec2(bodyW * 0.6f, bodyH - 20.0f));
                        const ImVec2 bMin = ImGui::GetItemRectMin();
                        const ImVec2 bMax = ImGui::GetItemRectMax();
                        const ImVec2 ts   = ImGui::CalcTextSize(node.label);
                        ImGui::GetWindowDrawList()->AddText(
                            ImVec2(bMin.x + (bMax.x - bMin.x - ts.x) * 0.5f,
                                   bMin.y + (bMax.y - bMin.y - ts.y) * 0.5f),
                            labelCol, node.label);
                        if (node.kind == FlowNodeKind_Start) {
                            s_emitSlot(slotOutput(node.id), ImVec2(0.5f, 1.0f));
                        }
                        ImNodal::EndNode();
                    }
                    ImNodal::PopStyleVar();
                    break;
                }

                case FlowNodeKind_IO: {
                    // Parallelogram — lib paints the polygon via SetNodeBodyShape.
                    if (ImNodal::BeginNode(node.id)) {
                        s_emitSlot(slotInput(node.id), ImVec2(0.5f, 0.0f));
                        const float slant = 16.0f;
                        ImGui::Dummy(ImVec2(bodyW + slant, bodyH - 20.0f));
                        const ImVec2 bMin = ImGui::GetItemRectMin();
                        const ImVec2 bMax = ImGui::GetItemRectMax();
                        static ImVec2 s_pts[4];
                        s_pts[0] = ImVec2(bMin.x + slant, bMin.y);
                        s_pts[1] = ImVec2(bMax.x,         bMin.y);
                        s_pts[2] = ImVec2(bMax.x - slant, bMax.y);
                        s_pts[3] = ImVec2(bMin.x,         bMax.y);
                        ImNodalHitbox shape;
                        shape.type          = ImNodalHitShape_ConvexPolygon;
                        shape.polygonPoints = s_pts;
                        shape.polygonCount  = 4;
                        ImNodal::SetNodeBodyShape(shape);
                        ImNodal::SetNodeHitbox(shape);
                        const ImVec2 ts = ImGui::CalcTextSize(node.label);
                        ImGui::GetWindowDrawList()->AddText(
                            ImVec2(bMin.x + (bMax.x - bMin.x - ts.x) * 0.5f,
                                   bMin.y + (bMax.y - bMin.y - ts.y) * 0.5f),
                            labelCol, node.label);
                        s_emitSlot(slotOutput(node.id), ImVec2(0.5f, 1.0f));
                        ImNodal::EndNode();
                    }
                    break;
                }

                case FlowNodeKind_Decision: {
                    // Diamond — lib paints the polygon via SetNodeBodyShape.
                    if (ImNodal::BeginNode(node.id)) {
                        s_emitSlot(slotInput(node.id), ImVec2(0.5f, 0.0f));
                        const float halfW = bodyW * 0.55f;
                        const float halfH = bodyH * 0.85f;
                        ImGui::Dummy(ImVec2(halfW * 2.0f, halfH * 2.0f));
                        const ImVec2 itemMin = ImGui::GetItemRectMin();
                        const ImVec2 c(itemMin.x + halfW, itemMin.y + halfH);
                        static ImVec2 s_pts[4];
                        s_pts[0] = ImVec2(c.x,         c.y - halfH);
                        s_pts[1] = ImVec2(c.x + halfW, c.y);
                        s_pts[2] = ImVec2(c.x,         c.y + halfH);
                        s_pts[3] = ImVec2(c.x - halfW, c.y);
                        ImNodalHitbox shape;
                        shape.type          = ImNodalHitShape_ConvexPolygon;
                        shape.polygonPoints = s_pts;
                        shape.polygonCount  = 4;
                        ImNodal::SetNodeBodyShape(shape);
                        ImNodal::SetNodeHitbox(shape);
                        const ImVec2 ts = ImGui::CalcTextSize(node.label);
                        ImGui::GetWindowDrawList()->AddText(
                            ImVec2(c.x - ts.x * 0.5f, c.y - ImGui::GetFontSize() * 0.5f),
                            labelCol, node.label);
                        // Two outputs : "No" on bottom-left, "Yes" on bottom-right.
                        // Pivot at the bottom-center of each slot dummy and use
                        // SameLine spacing so the pivots end up at the diamond's
                        // bottom-left / bottom-right corners (= halfW*2 apart).
                        s_emitSlot(slotOutput2(node.id), ImVec2(0.5f, 1.0f));
                        ImGui::SameLine(0.0f, halfW * 2.0f - 10.0f);
                        s_emitSlot(slotOutput (node.id), ImVec2(0.5f, 1.0f));
                        // "Yes" / "No" labels near the two outputs — painted
                        // inside the node scope so they land on the content
                        // channel like the central label above.
                        ImDrawList* pDL = ImGui::GetWindowDrawList();
                        const ImVec2 yesPos = ImNodal::GetSlotScreenPos(slotOutput (node.id));
                        const ImVec2 noPos  = ImNodal::GetSlotScreenPos(slotOutput2(node.id));
                        pDL->AddText(ImVec2(yesPos.x + 6.0f, yesPos.y -  2.0f), labelCol, "Yes");
                        pDL->AddText(ImVec2(noPos.x - 26.0f, noPos.y -  2.0f), labelCol, "No");
                        ImNodal::EndNode();
                    }
                    break;
                }

                case FlowNodeKind_COUNT: break;  // silence -Wswitch
            }
        }

        // ---------------------------
        // Draw links — Manhattan polyline + arrow head at the destination.
        // ---------------------------
        const ImU32 linkCol = ImNodal::GetStyleColorU32(ImNodalCol_Link);
        for (const FlowLink& link : s_links) {
            if (ImNodal::BeginLink(link.id, link.from, link.to)) {
                const ImVec2 p0 = ImNodal::GetLinkFromPos();
                const ImVec2 p1 = ImNodal::GetLinkToPos();
                const float midY = (p0.y + p1.y) * 0.5f;
                const ImVec2 pts[4] = {
                    p0,
                    ImVec2(p0.x, midY),
                    ImVec2(p1.x, midY),
                    p1,
                };
                ImNodal::LinkPolyline(pts, 4);
                // Arrow at p1, direction = last segment (pts[2] -> p1).
                s_paintArrow(ImGui::GetWindowDrawList(),
                             p1,
                             ImVec2(p1.x - pts[2].x, p1.y - pts[2].y),
                             linkCol);
                ImNodal::EndLink();
            }
        }

        // ---------------------------
        // Connection create — valid link OR dropped on empty canvas.
        // ---------------------------
        if (ImNodal::BeginConnectionCreate()) {
            Id from = 0;
            Id to   = 0;
            if (ImNodal::QueryNewLink(&from, &to)) {
                if (from != to) {
                    if (ImNodal::AcceptNewLink()) {
                        s_links.push_back({s_nextLinkId++, from, to});
                    }
                } else {
                    ImNodal::RejectNewLink("self-loop not allowed");
                }
            } else if (ImNodal::QueryNewNodeFromSlot(&from)) {
                if (ImNodal::AcceptNewNodeFromSlot()) {
                    s_dropFromSlot   = from;
                    s_dropPopupPos   = ImNodal::ScreenToCanvas(ImGui::GetIO().MousePos);
                    s_openDropPopup  = true;
                }
            }
            ImNodal::EndConnectionCreate();
        }

        // ---------------------------
        // Right-click on empty canvas → "Create node" popup.
        // ---------------------------
        if (ImNodal::IsCanvasContextMenuRequested()) {
            s_canvasPopupPos  = ImNodal::ScreenToCanvas(ImGui::GetIO().MousePos);
            s_openCanvasPopup = true;
        }

        ImNodal::EndGraph();
    }
    ImNodal::EndCanvas();

    // -----------------------------
    // Popups — emitted OUTSIDE the canvas so they float above it.
    // OpenPopup must be called from this frame's deferred path because
    // it captures the current ImGui ID stack — doing it inside the canvas
    // would scope the popup under the canvas child.
    // -----------------------------
    if (s_openCanvasPopup) {
        ImGui::OpenPopup("##flow_create_canvas");
        s_openCanvasPopup = false;
    }
    if (s_openDropPopup) {
        ImGui::OpenPopup("##flow_create_drop");
        s_openDropPopup = false;
    }

    // Shared kind-picker body — emits a MenuItem per kind, creates the node
    // (and the link if linkFromSlot != 0) at canvasPos when clicked.
    auto kindPicker = [&](ImVec2 aCanvasPos, Id aLinkFromSlot) {
        for (int i = 0; i < FlowNodeKind_COUNT; ++i) {
            if (ImGui::MenuItem(k_kindNames[i])) {
                const Id newId = s_nextNodeId++;
                s_nodes.push_back({newId, (FlowNodeKind)i, k_kindNames[i]});
                ImNodal::SetNextNodePos(newId, aCanvasPos);
                if (aLinkFromSlot != 0) {
                    s_links.push_back({s_nextLinkId++, aLinkFromSlot, slotInput(newId)});
                }
            }
        }
    };

    if (ImGui::BeginPopup("##flow_create_canvas")) {
        ImGui::TextDisabled("Create node");
        ImGui::Separator();
        kindPicker(s_canvasPopupPos, 0);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##flow_create_drop")) {
        ImGui::TextDisabled("Create connected node");
        ImGui::Separator();
        kindPicker(s_dropPopupPos, s_dropFromSlot);
        ImGui::EndPopup();
    }
}
