#include "flowChart.h"

#define IMGUI_DEFINE_MATH_OPERATORS
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

} // anonymous namespace

void FlowChart::display() {
    // -----------------------------
    // Persistent data — all static, sits next to the function body.
    // -----------------------------
    static std::vector<FlowNode> s_nodes;
    static std::vector<FlowLink> s_links;
    static Id s_nextNodeId = 100;
    static Id s_nextLinkId = 5000;
    static bool s_inited = false;

    // Popup staging — set by the canvas/graph logic, consumed by the
    // ImGui popups emitted outside the canvas scope.
    static bool s_openCanvasPopup = false;
    static ImVec2 s_canvasPopupPos = ImVec2(0.0f, 0.0f);
    static bool s_openDropPopup = false;
    static ImVec2 s_dropPopupPos = ImVec2(0.0f, 0.0f);
    static Id s_dropFromSlot = 0;

    // -----------------------------
    // Canvas + graph scope.
    // -----------------------------
    if (ImNodal::BeginCanvas("##flowchart_canvas", ImVec2(0.0f, 0.0f))) {
        if (ImNodal::BeginGraph(1)) {
            const float bodyW = 140.0f;
            const float bodyH = 60.0f;
            const ImU32 labelCol = IM_COL32(225, 230, 240, 255);

            // ---------------------------
            // Draw nodes — switch on kind, inline. No sub-function explosion.
            // ---------------------------
            for (const FlowNode& node : s_nodes) {
                switch (node.kind) {
                    case FlowNodeKind_Process:
                    case FlowNodeKind_PredefinedProcess: {
                        /*if (ImNodal::BeginNode(node.id)) {
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
                        }*/
                        break;
                    }

                    case FlowNodeKind_Start:
                    case FlowNodeKind_End: {
                        // Pill = rect with NodeRounding == bodyHeight / 2.
                        /*ImNodal::PushStyleVar(ImNodalStyleVar_NodeRounding, bodyH * 0.5f);
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
                        ImNodal::PopStyleVar();*/
                        break;
                    }

                    case FlowNodeKind_IO: {
                        // Parallelogram — lib paints the polygon via SetNodeBodyShape.
                        /*if (ImNodal::BeginNode(node.id)) {
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
                        }*/
                        break;
                    }

                    case FlowNodeKind_Decision: {
                        m_drawConditionalNode(node.label, node.id);
                        break;
                    }

                    case FlowNodeKind_COUNT:
                        break;  // silence -Wswitch
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
                    s_paintArrow(ImGui::GetWindowDrawList(), p1, ImVec2(p1.x - pts[2].x, p1.y - pts[2].y), linkCol);
                    ImNodal::EndLink();
                }
            }

            // ---------------------------
            // Connection create — valid link OR dropped on empty canvas.
            // ---------------------------
            if (ImNodal::BeginConnectionCreate()) {
                Id from = 0;
                Id to = 0;
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
                        s_dropFromSlot = from;
                        s_dropPopupPos = ImNodal::ScreenToCanvas(ImGui::GetIO().MousePos);
                        s_openDropPopup = true;
                    }
                }
                ImNodal::EndConnectionCreate();
            }

            // ---------------------------
            // Right-click on empty canvas → "Create node" popup.
            // ---------------------------
            if (ImNodal::IsCanvasContextMenuRequested()) {
                s_canvasPopupPos = ImNodal::ScreenToCanvas(ImGui::GetIO().MousePos);
                s_openCanvasPopup = true;
            }

            ImNodal::EndGraph();
        }
        ImNodal::EndCanvas();
    }
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
                    // s_links.push_back({s_nextLinkId++, aLinkFromSlot, slotInput(newId)});
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


void FlowChart::m_drawConditionalNode(const char* apLabel, const ImNodal::Id aNodeId) {
    if (ImNodal::BeginNode(aNodeId)) {
        ImGuiContext& g = *GImGui;
        ImGui::Text("%s", apLabel);
        auto rect = g.LastItemData.Rect;
        rect.Min.x -= 10.0f;
        rect.Min.y -= 10.0f;
        rect.Max.x += 10.0f;
        rect.Max.y += 10.0f;
        const auto center = rect.GetCenter();
        static ImVec2 s_node_pts[4];

        //   1
        //  / \
        // 0   2
        //  \ /
        //   3
        s_node_pts[0] = ImVec2(rect.Min.x, center.y);
        s_node_pts[1] = ImVec2(center.x, rect.Min.y);
        s_node_pts[2] = ImVec2(rect.Max.x, center.y);
        s_node_pts[3] = ImVec2(center.x, rect.Max.y);
        ImNodalHitbox node_shape;
        node_shape.type = ImNodalHitShape_ConvexPolygon;
        node_shape.polygonPoints = s_node_pts;
        node_shape.polygonCount = 4;
        ImNodal::SetNodeBodyShape(node_shape);

        // top : input
        const auto slotId = aNodeId * 100 + 1;
        if (ImNodal::BeginSlot(slotId)) {
            static ImVec2 s_slot_pts[3];
            s_slot_pts[0] = (s_node_pts[0] + s_node_pts[1]) * 0.5f;
            s_slot_pts[1] = s_node_pts[1];
            s_slot_pts[2] = (s_node_pts[1] + s_node_pts[2]) * 0.5f;
            ImNodalHitbox slot_shape;
            slot_shape.type = ImNodalHitShape_ConvexPolygon;
            slot_shape.polygonPoints = s_slot_pts;
            slot_shape.polygonCount = 3;
            ImNodal::SetSlotBodyShape(slot_shape);
            ImNodal::SetSlotAnchor(s_slot_pts[1]);
            ImNodal::SetSlotTangent(ImVec2(0.0f, -1.0f));
            // ImNodal::SetSlotHitbox(slot_shape);
            // const ImU32 col = ImNodal::IsSlotHovered(slotId) ? IM_COL32(255, 220, 120, 255) : (ImNodal::IsSlotConnected(slotId) ? IM_COL32(120, 200, 255, 255) : IM_COL32(100, 100, 100, 100));
            // ImGui::GetWindowDrawList()->AddConvexPolyFilled(slot_shape.polygonPoints, slot_shape.polygonCount, col);
            ImNodal::EndSlot();
        }

        ImNodal::EndNode();
    }
}
