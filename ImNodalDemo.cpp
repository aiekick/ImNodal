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

// ImNodalDemo — auto-contained showcase, mimics ImGui::ShowDemoWindow().
//
// Layout : MenuBar (Style submenu) + a vertical stack of CollapsingHeader
// sections, one per public feature (or feature group). Each section embeds
// its OWN live canvas + tweak widgets + code snippet. Sections are isolated
// from each other via unique canvas ids — they share the host's single
// ImNodal Context (Context = global, like ImGui).

#include "ImNodal.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace ImNodal {

namespace {

// =====================================================================
// Local helpers (private to the demo)
// =====================================================================

// Same idiom as ImGui's demo HelpMarker — a dim "(?)" with tooltip on hover.
static void s_helpMarker(const char* aDesc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(aDesc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Show a static code snippet in a subtle bordered box.
static void s_showCode(const char* aLabel, const char* aCode) {
    if (!ImGui::TreeNode(aLabel)) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(18, 18, 22, 220));
    const float textH = ImGui::GetTextLineHeightWithSpacing();
    int lineCount = 1;
    for (const char* p = aCode; *p; ++p) if (*p == '\n') ++lineCount;
    const float h = textH * (float)lineCount + ImGui::GetStyle().FramePadding.y * 2.0f;
    ImGui::BeginChild(ImGui::GetID(aCode), ImVec2(0.0f, h), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted(aCode);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::TreePop();
}

// Slot dot painter — drawn at the slot pivot AFTER EndSlot. Color reflects
// hover / connection. Reused by most sections.
static void s_paintSlotDot(Id aSlotId, float aRadius = 4.5f) {
    const ImVec2 c = ImNodal::GetSlotScreenPos(aSlotId);
    ImU32 col = IM_COL32(200, 200, 210, 255);
    if (ImNodal::IsSlotHovered(aSlotId)) {
        col = IM_COL32(255, 220, 120, 255);
    } else if (ImNodal::IsSlotConnected(aSlotId)) {
        col = IM_COL32(120, 200, 255, 255);
    }
    ImGui::GetWindowDrawList()->AddCircleFilled(c, aRadius, col);
}

// Persistent user-created link record. Each section that wants drag-from-slot
// link creation owns a vector<UserLink> + a monotonic id counter.
struct UserLink { Id id; Id from; Id to; };

// Run the connection-create state machine + commit accepted links into the
// caller's vector. Enforces "output (or InOut) -> input (or InOut)" — same
// rule most graph hosts want. Call this BETWEEN BeginGraph and EndGraph,
// AFTER emitting nodes/slots and the existing links. Returns true on the
// frame a new link was committed.
static bool s_runConnectionCreate(std::vector<UserLink>& aLinks, Id& aNextLinkId) {
    bool committed = false;
    if (ImNodal::BeginConnectionCreate()) {
        Id from, to;
        if (ImNodal::QueryNewLink(&from, &to)) {
            const SlotRole roleFrom = ImNodal::GetSlotRole(from);
            const SlotRole roleTo   = ImNodal::GetSlotRole(to);
            const bool ok = (roleFrom != SlotRole_Input) && (roleTo != SlotRole_Output);
            if (ok) {
                if (ImNodal::AcceptNewLink()) {
                    aLinks.push_back({aNextLinkId++, from, to});
                    committed = true;
                }
            } else {
                ImNodal::RejectNewLink("must be output -> input");
            }
        }
        ImNodal::EndConnectionCreate();
    }
    return committed;
}

// =====================================================================
// Section : Canvas basics — pan, zoom, grid, reset view
// =====================================================================
static void s_sectionCanvasBasics() {
    ImGui::TextWrapped(
        "BeginCanvas opens a pannable / zoomable rectangle. "
        "Middle-drag pans, wheel zooms (anchored on cursor), R resets zoom.");

    static bool s_drawGrid = true;
    static float s_gridSize[2]   = {50.0f, 50.0f};
    static float s_gridSubdivs[2] = {5.0f, 5.0f};
    static float s_zoomRange[2]  = {0.1f, 10.0f};

    ImGui::Checkbox("Auto-draw grid", &s_drawGrid);
    ImGui::SameLine();
    s_helpMarker("Toggle ImNodalCanvasFlags_NoGrid. When off, the host can still call "
                 "ImNodal::DrawCanvasGrid() manually for custom layering.");
    ImGui::SetNextItemWidth(180.0f);
    ImGui::DragFloat2("Grid size (px)",    s_gridSize,    0.5f, 4.0f, 256.0f, "%.0f");
    ImGui::SetNextItemWidth(180.0f);
    ImGui::DragFloat2("Grid subdivisions", s_gridSubdivs, 0.1f, 1.0f, 16.0f,  "%.0f");
    ImGui::SetNextItemWidth(180.0f);
    ImGui::DragFloat2("Zoom range",        s_zoomRange,   0.05f, 0.05f, 50.0f, "%.2f");

    if (ImGui::Button("Reset view")) {
        ImNodal::ResetCanvasView();
    }
    ImGui::SameLine();
    s_helpMarker("ResetCanvasView() re-centers origin and sets scale back to 1.");

    ImNodal::PushStyleVar(ImNodalStyleVar_GridSize,    ImVec2(s_gridSize[0],    s_gridSize[1]));
    ImNodal::PushStyleVar(ImNodalStyleVar_GridSubdivs, ImVec2(s_gridSubdivs[0], s_gridSubdivs[1]));

    ImNodal::CanvasSettings cs;
    cs.flags = s_drawGrid ? ImNodalCanvasFlags_None : ImNodalCanvasFlags_NoGrid;
    cs.zoomMin = s_zoomRange[0];
    cs.zoomMax = s_zoomRange[1];

    if (ImNodal::BeginCanvas("##canvas_basics", ImVec2(0.0f, 160.0f), cs)) {
        ImDrawList* pDL = ImGui::GetWindowDrawList();
        const ImU32 tagCol = IM_COL32(180, 180, 200, 220);
        pDL->AddText(ImVec2(   0.0f,    0.0f), tagCol, "(0, 0)");
        pDL->AddText(ImVec2( 200.0f,  100.0f), tagCol, "(200, 100)");
        pDL->AddText(ImVec2(-150.0f,   80.0f), tagCol, "(-150, 80)");
        pDL->AddText(ImVec2(  60.0f, -120.0f), tagCol, "(60, -120)");
        ImNodal::EndCanvas();
    }

    ImNodal::PopStyleVar(2);

    s_showCode("Show code",
        "ImNodal::CanvasSettings cs;\n"
        "cs.zoomMin = 0.1f;\n"
        "cs.zoomMax = 10.0f;\n"
        "ImNodal::PushStyleVar(ImNodalStyleVar_GridSize, ImVec2(50, 50));\n"
        "if (ImNodal::BeginCanvas(\"##my_canvas\", ImVec2(0, 160), cs)) {\n"
        "    // ... your nodes / widgets / drawings ...\n"
        "    ImNodal::EndCanvas();\n"
        "}\n"
        "ImNodal::PopStyleVar(1);\n");
}

// =====================================================================
// Section : Graph & multi-graph — two graphs sharing one canvas
// =====================================================================
static void s_sectionGraphMultiGraph() {
    ImGui::TextWrapped(
        "BeginGraph opens a graph scope inside a canvas. Several graphs can "
        "coexist in the same canvas — they share pan/zoom (the canvas's view) "
        "but keep their own selection, drag, and hover state.");

    static ImVec2 s_posA1 = ImVec2(-200.0f, -50.0f);
    static ImVec2 s_posA2 = ImVec2(-200.0f,  60.0f);
    static ImVec2 s_posB1 = ImVec2(  80.0f, -50.0f);
    static ImVec2 s_posB2 = ImVec2(  80.0f,  60.0f);

    if (ImNodal::BeginCanvas("##canvas_multigraph", ImVec2(0.0f, 220.0f))) {
        if (ImNodal::BeginGraph(1)) {
            if (ImNodal::BeginNode(101, &s_posA1)) {
                ImNodal::SetNodeColor(IM_COL32(80, 160, 220, 255));
                ImGui::TextUnformatted("A / node 1");
                ImNodal::EndNode();
            }
            if (ImNodal::BeginNode(102, &s_posA2)) {
                ImNodal::SetNodeColor(IM_COL32(80, 160, 220, 255));
                ImGui::TextUnformatted("A / node 2");
                ImNodal::EndNode();
            }
            ImNodal::EndGraph();
        }
        if (ImNodal::BeginGraph(2)) {
            if (ImNodal::BeginNode(201, &s_posB1)) {
                ImNodal::SetNodeColor(IM_COL32(220, 160, 80, 255));
                ImGui::TextUnformatted("B / node 1");
                ImNodal::EndNode();
            }
            if (ImNodal::BeginNode(202, &s_posB2)) {
                ImNodal::SetNodeColor(IM_COL32(220, 160, 80, 255));
                ImGui::TextUnformatted("B / node 2");
                ImNodal::EndNode();
            }
            ImNodal::EndGraph();
        }
        ImNodal::EndCanvas();
    }

    if (ImNodal::HasEditor("##canvas_multigraph", 1) && ImNodal::HasEditor("##canvas_multigraph", 2)) {
        ImNodal::SetCurrentEditor("##canvas_multigraph", 1);
        const Id selA = ImNodal::GetSelectedNode();
        ImNodal::SetCurrentEditor("##canvas_multigraph", 2);
        const Id selB = ImNodal::GetSelectedNode();
        ImGui::Text("Graph A — selected : %llu", (unsigned long long)selA);
        ImGui::SameLine(); s_helpMarker("Click a blue node : only graph A's selection changes. Orange graph stays untouched.");
        ImGui::Text("Graph B — selected : %llu", (unsigned long long)selB);
    }

    s_showCode("Show code",
        "if (ImNodal::BeginCanvas(\"##canvas\", size)) {\n"
        "    if (ImNodal::BeginGraph(1)) { /* nodes A */ ImNodal::EndGraph(); }\n"
        "    if (ImNodal::BeginGraph(2)) { /* nodes B */ ImNodal::EndGraph(); }\n"
        "    ImNodal::EndCanvas();\n"
        "}\n"
        "// Query a specific graph hors-scope :\n"
        "ImNodal::SetCurrentEditor(\"##canvas\", 1);\n"
        "Id selectedInA = ImNodal::GetSelectedNode();\n");
}

// =====================================================================
// Section : Nodes & Slots
// =====================================================================
static void s_sectionNodesSlots() {
    ImGui::TextWrapped(
        "BeginNode opens a draggable / selectable rect. BeginInputSlot / "
        "BeginOutputSlot reserve a hit area inside it ; the host paints "
        "the visible dot at GetSlotScreenPos AFTER EndSlot.");

    static ImVec2 s_posIn   = ImVec2(-180.0f, 0.0f);
    static ImVec2 s_posProc = ImVec2(   0.0f, 0.0f);
    static ImVec2 s_posOut  = ImVec2( 180.0f, 0.0f);
    static float s_nodeRounding = 4.0f;
    static float s_nodeBorderThickness = 1.5f;

    ImGui::SetNextItemWidth(180.0f);
    ImGui::DragFloat("NodeRounding",        &s_nodeRounding,        0.1f, 0.0f, 20.0f, "%.1f");
    ImGui::SetNextItemWidth(180.0f);
    ImGui::DragFloat("NodeBorderThickness", &s_nodeBorderThickness, 0.1f, 0.0f, 8.0f,  "%.1f");

    ImNodal::PushStyleVar(ImNodalStyleVar_NodeRounding,        s_nodeRounding);
    ImNodal::PushStyleVar(ImNodalStyleVar_NodeBorderThickness, s_nodeBorderThickness);

    static std::vector<UserLink> s_links;
    static Id s_nextLinkId = 5000;
    static bool s_inited = false;
    if (!s_inited) {
        s_links.push_back({1001, 11, 21});
        s_links.push_back({1002, 22, 31});
        s_inited = true;
    }

    if (ImNodal::BeginCanvas("##canvas_nodes_slots", ImVec2(0.0f, 220.0f))) {
        if (ImNodal::BeginGraph(1)) {
            if (ImNodal::BeginNode(1, &s_posIn)) {
                ImNodal::SetNodeColor(IM_COL32(80, 160, 220, 255));
                ImGui::TextUnformatted("Input");
                if (ImNodal::BeginOutputSlot(11)) {
                    ImGui::TextUnformatted("out");
                    ImNodal::EndSlot();
                    s_paintSlotDot(11);
                }
                ImNodal::EndNode();
            }
            if (ImNodal::BeginNode(2, &s_posProc)) {
                ImNodal::SetNodeColor(IM_COL32(160, 160, 160, 255));
                ImGui::TextUnformatted("Process");
                if (ImNodal::BeginInputSlot(21)) {
                    ImGui::TextUnformatted("in");
                    ImNodal::EndSlot();
                    s_paintSlotDot(21);
                }
                ImGui::SameLine(0.0f, 24.0f);
                if (ImNodal::BeginOutputSlot(22)) {
                    ImGui::TextUnformatted("out");
                    ImNodal::EndSlot();
                    s_paintSlotDot(22);
                }
                ImNodal::EndNode();
            }
            if (ImNodal::BeginNode(3, &s_posOut)) {
                ImNodal::SetNodeColor(IM_COL32(220, 160, 80, 255));
                ImGui::TextUnformatted("Output");
                if (ImNodal::BeginInputSlot(31)) {
                    ImGui::TextUnformatted("in");
                    ImNodal::EndSlot();
                    s_paintSlotDot(31);
                }
                ImNodal::EndNode();
            }
            for (auto& l : s_links) ImNodal::Link(l.id, l.from, l.to);
            s_runConnectionCreate(s_links, s_nextLinkId);
            ImNodal::EndGraph();
        }
        ImNodal::EndCanvas();
    }

    ImNodal::PopStyleVar(2);

    s_showCode("Show code",
        "if (ImNodal::BeginNode(nodeId, &pos)) {\n"
        "    ImNodal::SetNodeColor(IM_COL32(80, 160, 220, 255));\n"
        "    ImGui::TextUnformatted(\"Input\");\n"
        "    if (ImNodal::BeginOutputSlot(outId)) {\n"
        "        ImGui::TextUnformatted(\"out\");\n"
        "        ImNodal::EndSlot();\n"
        "        const ImVec2 c = ImNodal::GetSlotScreenPos(outId);\n"
        "        ImGui::GetWindowDrawList()->AddCircleFilled(c, 4.5f, color);\n"
        "    }\n"
        "    ImNodal::EndNode();\n"
        "}\n"
        "ImNodal::Link(linkId, outId, inId);\n");
}

// =====================================================================
// Section : Layout primitives
// =====================================================================
static void s_sectionLayoutPrimitives() {
    ImGui::TextWrapped(
        "BeginLayoutHorizontal/Vertical compose a node's content axis-by-axis. "
        "LayoutSpring distributes the remaining gap. BeginLayoutGroup wraps a "
        "bare ImGui widget so it counts as one layout child.");

    static ImVec2 s_pos = ImVec2(0.0f, 0.0f);

    if (ImNodal::BeginCanvas("##canvas_layout", ImVec2(0.0f, 220.0f))) {
        if (ImNodal::BeginGraph(1)) {
            if (ImNodal::BeginNode(1, &s_pos)) {
                ImNodal::SetNodeColor(IM_COL32(140, 200, 140, 255));
                ImNodal::BeginLayoutVertical("##node_root", ImVec2(-1.0f, 0.0f));
                    ImNodal::BeginLayoutHorizontal("##header", ImVec2(-1.0f, 0.0f));
                        ImNodal::LayoutSpring();
                        ImNodal::BeginLayoutGroup();
                            ImGui::TextUnformatted("Layout node");
                        ImNodal::EndLayoutGroup();
                        ImNodal::LayoutSpring();
                    ImNodal::EndLayoutHorizontal();
                    ImNodal::BeginLayoutHorizontal("##body", ImVec2(-1.0f, 0.0f));
                        ImNodal::BeginLayoutVertical("##inputs");
                            if (ImNodal::BeginInputSlot(11)) {
                                ImGui::TextUnformatted("A");
                                ImNodal::EndSlot();
                                s_paintSlotDot(11);
                            }
                            if (ImNodal::BeginInputSlot(12)) {
                                ImGui::TextUnformatted("B");
                                ImNodal::EndSlot();
                                s_paintSlotDot(12);
                            }
                        ImNodal::EndLayoutVertical();
                        ImNodal::LayoutSpring();
                        ImNodal::BeginLayoutVertical("##outputs");
                            if (ImNodal::BeginOutputSlot(21)) {
                                ImGui::TextUnformatted("Out");
                                ImNodal::EndSlot();
                                s_paintSlotDot(21);
                            }
                        ImNodal::EndLayoutVertical();
                    ImNodal::EndLayoutHorizontal();
                ImNodal::EndLayoutVertical();
                ImNodal::EndNode();
            }
            ImNodal::EndGraph();
        }
        ImNodal::EndCanvas();
    }

    s_showCode("Show code",
        "if (ImNodal::BeginNode(nodeId, &pos)) {\n"
        "    ImNodal::BeginLayoutVertical(\"##root\", ImVec2(-1, 0));\n"
        "    ImNodal::BeginLayoutHorizontal(\"##header\");\n"
        "        ImNodal::LayoutSpring();\n"
        "        ImNodal::BeginLayoutGroup();\n"
        "            ImGui::TextUnformatted(\"Title\");\n"
        "        ImNodal::EndLayoutGroup();\n"
        "        ImNodal::LayoutSpring();\n"
        "    ImNodal::EndLayoutHorizontal();\n"
        "    ImNodal::BeginLayoutHorizontal(\"##body\");\n"
        "        // ... input slots ...\n"
        "        ImNodal::LayoutSpring();\n"
        "        // ... output slots ...\n"
        "    ImNodal::EndLayoutHorizontal();\n"
        "    ImNodal::EndLayoutVertical();\n"
        "    ImNodal::EndNode();\n"
        "}\n");
}

// =====================================================================
// Section : Links — Bezier / Line / Polyline / Manhattan
// =====================================================================
static void s_sectionLinks() {
    ImGui::TextWrapped(
        "Link() is the default cubic-bezier. For custom shapes : "
        "BeginLink + LinkLineSegment / LinkBezierSegment / LinkPolyline + EndLink. "
        "Hit-test is chord-by-chord so any polyline works (including Manhattan).");

    static int s_style = 0;
    static const char* kStyleNames[] = {"Bezier (default)", "Straight line", "Manhattan", "Polyline (zig-zag)"};
    ImGui::SetNextItemWidth(220.0f);
    ImGui::Combo("Link style", &s_style, kStyleNames, IM_ARRAYSIZE(kStyleNames));

    static ImVec2 s_p0 = ImVec2(-180.0f, -40.0f);
    static ImVec2 s_p1 = ImVec2(-180.0f,  60.0f);
    static ImVec2 s_p2 = ImVec2( 180.0f, -40.0f);
    static ImVec2 s_p3 = ImVec2( 180.0f,  60.0f);

    auto drawLink = [&](Id linkId, Id from, Id to) {
        if (s_style == 0) {
            ImNodal::Link(linkId, from, to);
        } else if (ImNodal::BeginLink(linkId, from, to)) {
            const ImVec2 p0 = ImNodal::GetLinkFromPos();
            const ImVec2 p1 = ImNodal::GetLinkToPos();
            if (s_style == 1) {
                ImNodal::LinkLineSegment(p0, p1);
            } else if (s_style == 2) {
                const float midX = (p0.x + p1.x) * 0.5f;
                ImVec2 pts[4] = { p0, ImVec2(midX, p0.y), ImVec2(midX, p1.y), p1 };
                ImNodal::LinkPolyline(pts, 4);
            } else {
                const ImVec2 mid((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                ImVec2 pts[5] = {
                    p0,
                    ImVec2((p0.x + mid.x) * 0.5f, p0.y - 20.0f),
                    mid,
                    ImVec2((mid.x + p1.x) * 0.5f, p1.y + 20.0f),
                    p1,
                };
                ImNodal::LinkPolyline(pts, 5);
            }
            ImNodal::EndLink();
        }
    };

    if (ImNodal::BeginCanvas("##canvas_links", ImVec2(0.0f, 240.0f))) {
        if (ImNodal::BeginGraph(1)) {
            auto emitOut = [&](Id nodeId, Id slotId, ImVec2* pos, const char* label) {
                if (ImNodal::BeginNode(nodeId, pos)) {
                    ImGui::TextUnformatted(label);
                    if (ImNodal::BeginOutputSlot(slotId)) {
                        ImGui::TextUnformatted("out"); ImNodal::EndSlot(); s_paintSlotDot(slotId);
                    }
                    ImNodal::EndNode();
                }
            };
            auto emitIn = [&](Id nodeId, Id slotId, ImVec2* pos, const char* label) {
                if (ImNodal::BeginNode(nodeId, pos)) {
                    ImGui::TextUnformatted(label);
                    if (ImNodal::BeginInputSlot(slotId)) {
                        ImGui::TextUnformatted("in"); ImNodal::EndSlot(); s_paintSlotDot(slotId);
                    }
                    ImNodal::EndNode();
                }
            };
            emitOut(1, 11, &s_p0, "A");
            emitOut(2, 12, &s_p1, "B");
            emitIn (3, 21, &s_p2, "X");
            emitIn (4, 22, &s_p3, "Y");
            static std::vector<UserLink> s_links;
            static Id s_nextLinkId = 5000;
            static bool s_inited = false;
            if (!s_inited) {
                s_links.push_back({1001, 11, 21});
                s_links.push_back({1002, 12, 22});
                s_inited = true;
            }
            for (auto& l : s_links) drawLink(l.id, l.from, l.to);
            s_runConnectionCreate(s_links, s_nextLinkId);
            ImNodal::EndGraph();
        }
        ImNodal::EndCanvas();
    }

    s_showCode("Show code",
        "if (ImNodal::BeginLink(linkId, fromSlot, toSlot)) {\n"
        "    ImVec2 p0 = ImNodal::GetLinkFromPos();\n"
        "    ImVec2 p1 = ImNodal::GetLinkToPos();\n"
        "    // Manhattan : 3 segments through a vertical mid X.\n"
        "    const float midX = (p0.x + p1.x) * 0.5f;\n"
        "    ImVec2 pts[4] = { p0, ImVec2(midX, p0.y), ImVec2(midX, p1.y), p1 };\n"
        "    ImNodal::LinkPolyline(pts, 4);\n"
        "    ImNodal::EndLink();\n"
        "}\n");
}

// =====================================================================
// Section : Flow animation
// =====================================================================
static void s_sectionFlowAnimation() {
    ImGui::TextWrapped(
        "FlowLink(linkId, speed) animates dots along a link's cached "
        "polyline. Call it RIGHT AFTER the matching Link() so the path "
        "is already in the buffer.");

    static float s_speed = 2.0f;
    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderFloat("Speed (canvas units/s)", &s_speed, 0.1f, 10.0f, "%.2f");

    static ImVec2 s_pA = ImVec2(-150.0f, 0.0f);
    static ImVec2 s_pB = ImVec2( 150.0f, 0.0f);

    if (ImNodal::BeginCanvas("##canvas_flow", ImVec2(0.0f, 200.0f))) {
        if (ImNodal::BeginGraph(1)) {
            if (ImNodal::BeginNode(1, &s_pA)) {
                ImGui::TextUnformatted("Source");
                if (ImNodal::BeginOutputSlot(11)) { ImGui::TextUnformatted("out"); ImNodal::EndSlot(); s_paintSlotDot(11); }
                ImNodal::EndNode();
            }
            if (ImNodal::BeginNode(2, &s_pB)) {
                ImGui::TextUnformatted("Sink");
                if (ImNodal::BeginInputSlot(21)) { ImGui::TextUnformatted("in"); ImNodal::EndSlot(); s_paintSlotDot(21); }
                ImNodal::EndNode();
            }
            static std::vector<UserLink> s_links;
            static Id s_nextLinkId = 5000;
            static bool s_inited = false;
            if (!s_inited) {
                s_links.push_back({1001, 11, 21});
                s_inited = true;
            }
            for (auto& l : s_links) {
                ImNodal::Link(l.id, l.from, l.to);
                ImNodal::FlowLink(l.id, s_speed);
            }
            s_runConnectionCreate(s_links, s_nextLinkId);
            ImNodal::EndGraph();
        }
        ImNodal::EndCanvas();
    }

    s_showCode("Show code",
        "ImNodal::Link(linkId, fromSlot, toSlot);\n"
        "ImNodal::FlowLink(linkId, /*speed=*/2.0f);\n");
}

// =====================================================================
// Section : Connection Create
// =====================================================================
static void s_sectionConnectionCreate() {
    ImGui::TextWrapped(
        "Drag from a slot to start a connection. BeginConnectionCreate / "
        "QueryNewLink let the host validate the target ; AcceptNewLink "
        "commits on release (returns true once), RejectNewLink paints the "
        "preview red. Direction is enforced here : output -> input only.");

    static std::vector<UserLink> s_links;
    static Id s_nextLinkId = 5000;
    static char s_lastEvent[96] = "(idle)";

    if (ImGui::Button("Clear all links")) {
        s_links.clear();
        std::snprintf(s_lastEvent, sizeof(s_lastEvent), "cleared");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Last event : %s", s_lastEvent);

    static ImVec2 s_pA = ImVec2(-180.0f, -40.0f);
    static ImVec2 s_pB = ImVec2(-180.0f,  60.0f);
    static ImVec2 s_pC = ImVec2( 180.0f, -40.0f);
    static ImVec2 s_pD = ImVec2( 180.0f,  60.0f);

    if (ImNodal::BeginCanvas("##canvas_concreate", ImVec2(0.0f, 240.0f))) {
        if (ImNodal::BeginGraph(1)) {
            if (ImNodal::BeginNode(1, &s_pA)) {
                ImGui::TextUnformatted("Out 1");
                if (ImNodal::BeginOutputSlot(11)) { ImGui::TextUnformatted("o"); ImNodal::EndSlot(); s_paintSlotDot(11); }
                ImNodal::EndNode();
            }
            if (ImNodal::BeginNode(2, &s_pB)) {
                ImGui::TextUnformatted("Out 2");
                if (ImNodal::BeginOutputSlot(12)) { ImGui::TextUnformatted("o"); ImNodal::EndSlot(); s_paintSlotDot(12); }
                ImNodal::EndNode();
            }
            if (ImNodal::BeginNode(3, &s_pC)) {
                ImGui::TextUnformatted("In 1");
                if (ImNodal::BeginInputSlot(21)) { ImGui::TextUnformatted("i"); ImNodal::EndSlot(); s_paintSlotDot(21); }
                ImNodal::EndNode();
            }
            if (ImNodal::BeginNode(4, &s_pD)) {
                ImGui::TextUnformatted("In 2");
                if (ImNodal::BeginInputSlot(22)) { ImGui::TextUnformatted("i"); ImNodal::EndSlot(); s_paintSlotDot(22); }
                ImNodal::EndNode();
            }
            for (auto& l : s_links) {
                ImNodal::Link(l.id, l.from, l.to);
            }
            if (ImNodal::BeginConnectionCreate()) {
                Id fromSlot, toSlot;
                if (ImNodal::QueryNewLink(&fromSlot, &toSlot)) {
                    const bool fromIsOutput = ImNodal::GetSlotRole(fromSlot) == SlotRole_Output;
                    const bool toIsInput    = ImNodal::GetSlotRole(toSlot)   == SlotRole_Input;
                    if (fromIsOutput && toIsInput) {
                        if (ImNodal::AcceptNewLink()) {
                            s_links.push_back({s_nextLinkId++, fromSlot, toSlot});
                            std::snprintf(s_lastEvent, sizeof(s_lastEvent),
                                          "committed %llu -> %llu",
                                          (unsigned long long)fromSlot, (unsigned long long)toSlot);
                        }
                    } else {
                        ImNodal::RejectNewLink("direction mismatch");
                    }
                }
                ImNodal::EndConnectionCreate();
            }
            ImNodal::EndGraph();
        }
        ImNodal::EndCanvas();
    }

    s_showCode("Show code",
        "if (ImNodal::BeginConnectionCreate()) {\n"
        "    Id fromSlot, toSlot;\n"
        "    if (ImNodal::QueryNewLink(&fromSlot, &toSlot)) {\n"
        "        if (compatible(fromSlot, toSlot)) {\n"
        "            if (ImNodal::AcceptNewLink()) {\n"
        "                commit_link(fromSlot, toSlot);\n"
        "            }\n"
        "        } else {\n"
        "            ImNodal::RejectNewLink(\"reason\");\n"
        "        }\n"
        "    }\n"
        "    ImNodal::EndConnectionCreate();\n"
        "}\n");
}

// =====================================================================
// Section : Selection — single / multi / box-select
// =====================================================================
static void s_sectionSelection() {
    ImGui::TextWrapped(
        "Click selects, Shift+click toggles, drag from empty canvas opens a "
        "box-select. Live mode rebuilds the selection every frame while the "
        "box grows ; deferred (default) commits on release.");

    static bool s_boxSelectLive = false;
    ImGui::Checkbox("Box-select live", &s_boxSelectLive);
    ImGui::SameLine();
    s_helpMarker("ImNodalGraphFlags_BoxSelectLive.");

    const bool hasGraph = ImNodal::HasEditor("##canvas_selection", 1);

    if (ImGui::Button("Select all") && hasGraph) {
        ImNodal::SetCurrentEditor("##canvas_selection", 1);
        ImNodal::AddToSelection(1);
        ImNodal::AddToSelection(2);
        ImNodal::AddToSelection(3);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear selection") && hasGraph) {
        ImNodal::SetCurrentEditor("##canvas_selection", 1);
        ImNodal::ClearSelection();
    }
    ImGui::SameLine();
    if (hasGraph) {
        ImNodal::SetCurrentEditor("##canvas_selection", 1);
        ImGui::TextDisabled("Selected : %d", ImNodal::GetSelectedObjectCount());
    } else {
        ImGui::TextDisabled("Selected : -");
    }

    static ImVec2 s_p1 = ImVec2(-160.0f, -50.0f);
    static ImVec2 s_p2 = ImVec2(   0.0f,   0.0f);
    static ImVec2 s_p3 = ImVec2( 160.0f,  50.0f);

    GraphSettings gs;
    if (s_boxSelectLive) gs.flags |= ImNodalGraphFlags_BoxSelectLive;

    if (ImNodal::BeginCanvas("##canvas_selection", ImVec2(0.0f, 220.0f))) {
        if (ImNodal::BeginGraph(1, gs)) {
            if (ImNodal::BeginNode(1, &s_p1)) {
                ImGui::TextUnformatted("Alpha");
                if (ImNodal::BeginOutputSlot(11)) { ImGui::TextUnformatted("o"); ImNodal::EndSlot(); s_paintSlotDot(11); }
                ImNodal::EndNode();
            }
            if (ImNodal::BeginNode(2, &s_p2)) {
                ImGui::TextUnformatted("Beta");
                if (ImNodal::BeginInputSlot(21)) { ImGui::TextUnformatted("i"); ImNodal::EndSlot(); s_paintSlotDot(21); }
                ImGui::SameLine(0.0f, 20.0f);
                if (ImNodal::BeginOutputSlot(22)) { ImGui::TextUnformatted("o"); ImNodal::EndSlot(); s_paintSlotDot(22); }
                ImNodal::EndNode();
            }
            if (ImNodal::BeginNode(3, &s_p3)) {
                ImGui::TextUnformatted("Gamma");
                if (ImNodal::BeginInputSlot(31)) { ImGui::TextUnformatted("i"); ImNodal::EndSlot(); s_paintSlotDot(31); }
                ImNodal::EndNode();
            }
            static std::vector<UserLink> s_links;
            static Id s_nextLinkId = 5000;
            static bool s_inited = false;
            if (!s_inited) {
                s_links.push_back({1001, 11, 21});
                s_links.push_back({1002, 22, 31});
                s_inited = true;
            }
            for (auto& l : s_links) ImNodal::Link(l.id, l.from, l.to);
            s_runConnectionCreate(s_links, s_nextLinkId);
            ImNodal::EndGraph();
        }
        ImNodal::EndCanvas();
    }

    s_showCode("Show code",
        "ImNodal::GraphSettings gs;\n"
        "gs.flags |= ImNodalGraphFlags_BoxSelectLive;\n"
        "if (ImNodal::BeginGraph(graphId, gs)) {\n"
        "    // ... nodes, links ...\n"
        "    ImNodal::EndGraph();\n"
        "}\n"
        "int n = ImNodal::GetSelectedObjectCount();\n"
        "Id nodes[64]; int cnt = ImNodal::GetSelectedNodes(nodes, 64);\n");
}

// =====================================================================
// Section : Delete & Shortcuts
// =====================================================================
static void s_sectionDeleteShortcuts() {
    ImGui::TextWrapped(
        "BeginDelete fires on Delete or Backspace while the canvas is hovered ; "
        "QueryDeletedLink / QueryDeletedNode walk the selection. BeginShortcut "
        "catches Ctrl+C/V/X/D/A. Both let the host Accept / Reject each candidate.");

    static std::vector<UserLink> s_links;
    static Id s_nextLinkId = 5000;
    static bool s_inited = false;
    if (!s_inited) {
        s_links.push_back({2001, 11, 21});
        s_links.push_back({2002, 22, 31});
        s_inited = true;
    }
    static char s_lastEvent[96] = "(idle)";

    if (ImGui::Button("Re-add demo links")) {
        s_links.clear();
        s_links.push_back({2001, 11, 21});
        s_links.push_back({2002, 22, 31});
        std::snprintf(s_lastEvent, sizeof(s_lastEvent), "links re-added");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Last event : %s", s_lastEvent);
    ImGui::TextDisabled("Hover the canvas, select something, press Delete OR Ctrl+C/V/X/D/A.");

    static ImVec2 s_p1 = ImVec2(-150.0f, -30.0f);
    static ImVec2 s_p2 = ImVec2(   0.0f,  30.0f);
    static ImVec2 s_p3 = ImVec2( 150.0f, -30.0f);

    if (ImNodal::BeginCanvas("##canvas_delete", ImVec2(0.0f, 220.0f))) {
        if (ImNodal::BeginGraph(1)) {
            if (ImNodal::BeginNode(1, &s_p1)) {
                ImGui::TextUnformatted("A");
                if (ImNodal::BeginOutputSlot(11)) { ImGui::TextUnformatted("o"); ImNodal::EndSlot(); s_paintSlotDot(11); }
                ImNodal::EndNode();
            }
            if (ImNodal::BeginNode(2, &s_p2)) {
                ImGui::TextUnformatted("B");
                if (ImNodal::BeginInputSlot(21)) { ImGui::TextUnformatted("i"); ImNodal::EndSlot(); s_paintSlotDot(21); }
                ImGui::SameLine(0.0f, 20.0f);
                if (ImNodal::BeginOutputSlot(22)) { ImGui::TextUnformatted("o"); ImNodal::EndSlot(); s_paintSlotDot(22); }
                ImNodal::EndNode();
            }
            if (ImNodal::BeginNode(3, &s_p3)) {
                ImGui::TextUnformatted("C");
                if (ImNodal::BeginInputSlot(31)) { ImGui::TextUnformatted("i"); ImNodal::EndSlot(); s_paintSlotDot(31); }
                ImNodal::EndNode();
            }
            for (auto& l : s_links) {
                ImNodal::Link(l.id, l.from, l.to);
            }
            s_runConnectionCreate(s_links, s_nextLinkId);
            if (ImNodal::BeginDelete()) {
                Id linkId;
                while (ImNodal::QueryDeletedLink(&linkId)) {
                    auto newEnd = std::remove_if(s_links.begin(), s_links.end(),
                                                 [linkId](const UserLink& l) { return l.id == linkId; });
                    if (newEnd != s_links.end()) {
                        s_links.erase(newEnd, s_links.end());
                        ImNodal::AcceptDelete();
                        std::snprintf(s_lastEvent, sizeof(s_lastEvent), "deleted link %llu", (unsigned long long)linkId);
                    } else {
                        ImNodal::RejectDelete();
                    }
                }
                Id nodeId;
                while (ImNodal::QueryDeletedNode(&nodeId)) {
                    // We refuse node deletion to keep the demo's 3 anchors.
                    ImNodal::RejectDelete();
                    std::snprintf(s_lastEvent, sizeof(s_lastEvent), "node delete rejected (demo)");
                }
                ImNodal::EndDelete();
            }
            if (ImNodal::BeginShortcut()) {
                if      (ImNodal::AcceptCopy())      std::snprintf(s_lastEvent, sizeof(s_lastEvent), "Ctrl+C (copy)");
                else if (ImNodal::AcceptPaste())     std::snprintf(s_lastEvent, sizeof(s_lastEvent), "Ctrl+V (paste)");
                else if (ImNodal::AcceptCut())       std::snprintf(s_lastEvent, sizeof(s_lastEvent), "Ctrl+X (cut)");
                else if (ImNodal::AcceptDuplicate()) std::snprintf(s_lastEvent, sizeof(s_lastEvent), "Ctrl+D (duplicate)");
                else if (ImNodal::AcceptSelectAll()) std::snprintf(s_lastEvent, sizeof(s_lastEvent), "Ctrl+A (select all)");
                ImNodal::EndShortcut();
            }
            ImNodal::EndGraph();
        }
        ImNodal::EndCanvas();
    }

    s_showCode("Show code",
        "if (ImNodal::BeginDelete()) {\n"
        "    Id linkId;\n"
        "    while (ImNodal::QueryDeletedLink(&linkId)) {\n"
        "        remove_from_host_storage(linkId);\n"
        "        ImNodal::AcceptDelete();\n"
        "    }\n"
        "    Id nodeId;\n"
        "    while (ImNodal::QueryDeletedNode(&nodeId)) {\n"
        "        ImNodal::AcceptDelete();\n"
        "    }\n"
        "    ImNodal::EndDelete();\n"
        "}\n"
        "\n"
        "if (ImNodal::BeginShortcut()) {\n"
        "    if (ImNodal::AcceptCopy()) host_copy();\n"
        "    if (ImNodal::AcceptPaste()) host_paste();\n"
        "    ImNodal::EndShortcut();\n"
        "}\n");
}

// =====================================================================
// Section : MiniMap
// =====================================================================
static void s_sectionMiniMap() {
    ImGui::TextWrapped(
        "ShowMiniMap draws a scaled overview anchored to a corner of the canvas. "
        "Cursor over the minimap blocks ALL graph interactions ; click-drag "
        "recenters (UE-style), wheel zooms.");

    static int s_corner = 1;
    static const char* kCornerNames[] = {"Top-left", "Top-right", "Bottom-left", "Bottom-right"};
    ImGui::SetNextItemWidth(180.0f);
    ImGui::Combo("Anchor corner", &s_corner, kCornerNames, IM_ARRAYSIZE(kCornerNames));
    static float s_size[2] = {180.0f, 120.0f};
    ImGui::SetNextItemWidth(220.0f);
    ImGui::DragFloat2("MiniMap size", s_size, 1.0f, 60.0f, 320.0f, "%.0f");

    static ImVec2 s_pos[5] = {
        ImVec2(-220.0f, -80.0f),
        ImVec2( -80.0f,  60.0f),
        ImVec2(  80.0f, -40.0f),
        ImVec2( 220.0f,  80.0f),
        ImVec2(   0.0f,   0.0f),
    };
    static const ImU32 kCols[5] = {
        IM_COL32(220,  80,  80, 255),
        IM_COL32( 80, 220,  80, 255),
        IM_COL32( 80,  80, 220, 255),
        IM_COL32(220, 220,  80, 255),
        IM_COL32(220,  80, 220, 255),
    };

    if (ImNodal::BeginCanvas("##canvas_minimap", ImVec2(0.0f, 280.0f))) {
        if (ImNodal::BeginGraph(1)) {
            for (int i = 0; i < 5; ++i) {
                if (ImNodal::BeginNode((Id)(i + 1), &s_pos[i])) {
                    ImNodal::SetNodeColor(kCols[i]);
                    ImGui::Text("N%d", i + 1);
                    ImNodal::EndNode();
                }
            }
            ImNodal::MiniMapSettings ms;
            ms.size = ImVec2(s_size[0], s_size[1]);
            ms.anchor = (ImNodalCorner)s_corner;
            ImNodal::ShowMiniMap(ms);
            ImNodal::EndGraph();
        }
        ImNodal::EndCanvas();
    }

    s_showCode("Show code",
        "if (ImNodal::BeginGraph(graphId)) {\n"
        "    // ... emit nodes ...\n"
        "    ImNodal::MiniMapSettings ms;\n"
        "    ms.size = ImVec2(180, 120);\n"
        "    ms.anchor = ImNodalCorner_TopRight;\n"
        "    ImNodal::ShowMiniMap(ms);\n"
        "    ImNodal::EndGraph();\n"
        "}\n");
}

// =====================================================================
// Section : Custom Hitbox — flowchart diamond
// =====================================================================
// Diamonds with a slot at each corner. Both the NODE and the SLOTS use a
// ConvexPolygon hitbox that matches their visible shape exactly — that's
// the point of the section: the hover area follows the geometry, not an
// arbitrary AABB.
static void s_sectionDiamond() {
    ImGui::TextWrapped(
        "Filled diamond-shaped nodes with a small diamond slot at each "
        "corner. Node AND slot both use SetNodeHitbox / SetSlotHitbox with "
        "ConvexPolygon — the hover region matches the painted shape exactly. "
        "Hover a corner slot : the highlight follows the polygon, not a circle.");

    static float s_halfW = 60.0f;
    static float s_halfH = 40.0f;
    ImGui::SetNextItemWidth(180.0f);
    ImGui::DragFloat("Diamond half-width",  &s_halfW, 0.5f, 20.0f, 120.0f, "%.0f");
    ImGui::SetNextItemWidth(180.0f);
    ImGui::DragFloat("Diamond half-height", &s_halfH, 0.5f, 15.0f,  80.0f, "%.0f");

    static ImVec2 s_pA = ImVec2(-180.0f, -50.0f);
    static ImVec2 s_pB = ImVec2(  80.0f,  50.0f);

    auto emitDiamond = [&](Id aNodeId, ImVec2* apPos, const char* aLabel,
                           Id aN, Id aE, Id aS, Id aW,
                           SlotRole aRoleN, SlotRole aRoleE, SlotRole aRoleS, SlotRole aRoleW) {
        NodeSettings ns;
        ns.flags = ImNodalNodeFlags_NoBody | ImNodalNodeFlags_HiddenInMinimap;
        if (!ImNodal::BeginNode(aNodeId, apPos, ns)) return;

        // Reserve a rect of (2*halfW, 2*halfH) so the layout knows the
        // node's footprint.
        ImGui::Dummy(ImVec2(s_halfW * 2.0f, s_halfH * 2.0f));
        const ImVec2 itemMin = ImGui::GetItemRectMin();
        const ImVec2 center(itemMin.x + s_halfW, itemMin.y + s_halfH);
        const ImVec2 top   (center.x,           center.y - s_halfH);
        const ImVec2 right (center.x + s_halfW, center.y          );
        const ImVec2 bottom(center.x,           center.y + s_halfH);
        const ImVec2 left  (center.x - s_halfW, center.y          );

        // Big diamond polygon. Static storage : SetNodeHitbox keeps the
        // pointer alive until EndNode reads it for hit-testing. The
        // drawing primitives below COPY into the vertex buffer so they
        // don't care about lifetime.
        static ImVec2 s_nodePts[4];
        s_nodePts[0] = top;
        s_nodePts[1] = right;
        s_nodePts[2] = bottom;
        s_nodePts[3] = left;

        auto* pDL = ImGui::GetWindowDrawList();
        const bool nodeSelected = ImNodal::IsNodeSelected(aNodeId);
        const ImU32 fillCol    = IM_COL32(40, 60, 100, 230);
        const ImU32 outlineCol = nodeSelected
            ? IM_COL32(255, 200, 80, 255)
            : IM_COL32(200, 220, 255, 255);
        pDL->AddConvexPolyFilled(s_nodePts, 4, fillCol);
        // ImGui 1.92.8 swapped AddPolyline's flags/thickness — new order is
        // (points, count, col, thickness, flags).
        pDL->AddPolyline(s_nodePts, 4, outlineCol, 2.0f, ImDrawFlags_Closed);
        const ImVec2 textSize = ImGui::CalcTextSize(aLabel);
        pDL->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y - ImGui::GetFontSize() * 0.5f),
                     outlineCol, aLabel);

        // Node hitbox = the diamond polygon itself.
        ImNodalHitbox nodeHit;
        nodeHit.type = ImNodalHitShape_ConvexPolygon;
        nodeHit.polygonPoints = s_nodePts;
        nodeHit.polygonCount = 4;
        ImNodal::SetNodeHitbox(nodeHit);

        // Each corner is a slot drawn as a small diamond (4-pt convex
        // polygon). The slot hitbox MATCHES the painted polygon exactly —
        // when hovered, the polygon fill switches color, so the visual
        // hover area is literally the hitbox.
        auto emitCornerSlot = [&](Id aSlotId, const ImVec2& aCornerPos, SlotRole aRole) {
            const bool opened = (aRole == SlotRole_Input)
                ? ImNodal::BeginInputSlot(aSlotId)
                : ImNodal::BeginOutputSlot(aSlotId);
            if (!opened) return;

            // Mini-diamond around the corner. Static : alive between
            // SetSlotHitbox and EndSlot (same-call window). Sharing the
            // buffer across the 4 corner calls is safe because each pair
            // SetSlotHitbox+EndSlot is atomic w.r.t. its content.
            const float r = 8.0f;
            static ImVec2 s_slotPts[4];
            s_slotPts[0] = ImVec2(aCornerPos.x,     aCornerPos.y - r);
            s_slotPts[1] = ImVec2(aCornerPos.x + r, aCornerPos.y    );
            s_slotPts[2] = ImVec2(aCornerPos.x,     aCornerPos.y + r);
            s_slotPts[3] = ImVec2(aCornerPos.x - r, aCornerPos.y    );

            ImNodalHitbox slotHit;
            slotHit.type = ImNodalHitShape_ConvexPolygon;
            slotHit.polygonPoints = s_slotPts;
            slotHit.polygonCount = 4;
            ImNodal::SetSlotHitbox(slotHit);
            ImNodal::EndSlot();

            // Visual = same polygon as the hitbox. Fill color reflects
            // hover / connected state ; outline is constant.
            const ImU32 slotFillCol = ImNodal::IsSlotHovered(aSlotId)
                ? IM_COL32(255, 220, 120, 255)
                : (ImNodal::IsSlotConnected(aSlotId)
                    ? IM_COL32(120, 200, 255, 255)
                    : IM_COL32(200, 200, 210, 255));
            pDL->AddConvexPolyFilled(s_slotPts, 4, slotFillCol);
            pDL->AddPolyline(s_slotPts, 4, IM_COL32(20, 30, 50, 255), 1.0f, ImDrawFlags_Closed);
        };
        emitCornerSlot(aN, top,    aRoleN);
        emitCornerSlot(aE, right,  aRoleE);
        emitCornerSlot(aS, bottom, aRoleS);
        emitCornerSlot(aW, left,   aRoleW);

        ImNodal::EndNode();
    };

    static std::vector<UserLink> s_links;
    static Id s_nextLinkId = 5000;
    static bool s_inited = false;
    if (!s_inited) {
        s_links.push_back({1001, 12, 24});  // East-of-A → West-of-B
        s_inited = true;
    }

    if (ImNodal::BeginCanvas("##canvas_diamond", ImVec2(0.0f, 260.0f))) {
        if (ImNodal::BeginGraph(1)) {
            // Diamond A : N = in, E = out, S = out, W = in
            emitDiamond(1, &s_pA, "If",   11, 12, 13, 14,
                        SlotRole_Input, SlotRole_Output, SlotRole_Output, SlotRole_Input);
            emitDiamond(2, &s_pB, "Then", 21, 22, 23, 24,
                        SlotRole_Input, SlotRole_Output, SlotRole_Output, SlotRole_Input);
            // Manhattan links between corners : flowcharts use orthogonal
            // wires. 4-point polyline through a vertical mid X — LinkPolyline
            // does the hit-test chord-by-chord. User-created links get drawn
            // the same way for visual consistency.
            for (auto& l : s_links) {
                if (ImNodal::BeginLink(l.id, l.from, l.to)) {
                    const ImVec2 p0 = ImNodal::GetLinkFromPos();
                    const ImVec2 p1 = ImNodal::GetLinkToPos();
                    const float midX = (p0.x + p1.x) * 0.5f;
                    const ImVec2 pts[4] = { p0, ImVec2(midX, p0.y), ImVec2(midX, p1.y), p1 };
                    ImNodal::LinkPolyline(pts, 4);
                    ImNodal::EndLink();
                }
            }
            s_runConnectionCreate(s_links, s_nextLinkId);
            ImNodal::EndGraph();
        }
        ImNodal::EndCanvas();
    }

    s_showCode("Show code",
        "ImNodal::NodeSettings ns;\n"
        "ns.flags = ImNodalNodeFlags_NoBody | ImNodalNodeFlags_HiddenInMinimap;\n"
        "if (ImNodal::BeginNode(nodeId, &pos, ns)) {\n"
        "    ImGui::Dummy(ImVec2(2*halfW, 2*halfH));  // reserve footprint\n"
        "    ImVec2 nodePts[4] = { top, right, bottom, left };\n"
        "    pDL->AddConvexPolyFilled(nodePts, 4, fillCol);\n"
        "    pDL->AddPolyline(nodePts, 4, outlineCol, 2.0f, ImDrawFlags_Closed);\n"
        "    ImNodalHitbox h;\n"
        "    h.type = ImNodalHitShape_ConvexPolygon;\n"
        "    h.polygonPoints = nodePts; h.polygonCount = 4;\n"
        "    ImNodal::SetNodeHitbox(h);\n"
        "\n"
        "    if (ImNodal::BeginInputSlot(slotN_id)) {\n"
        "        ImVec2 slotPts[4] = {  // mini-diamond around the corner\n"
        "            ImVec2(c.x,   c.y-r), ImVec2(c.x+r, c.y),\n"
        "            ImVec2(c.x,   c.y+r), ImVec2(c.x-r, c.y),\n"
        "        };\n"
        "        ImNodalHitbox sh;\n"
        "        sh.type = ImNodalHitShape_ConvexPolygon;\n"
        "        sh.polygonPoints = slotPts; sh.polygonCount = 4;\n"
        "        ImNodal::SetSlotHitbox(sh);\n"
        "        ImNodal::EndSlot();\n"
        "        // Paint the slot polygon — visual hover = hitbox.\n"
        "        const ImU32 col = IsSlotHovered(slotN_id) ? hoverCol : restCol;\n"
        "        pDL->AddConvexPolyFilled(slotPts, 4, col);\n"
        "    }\n"
        "    // ... E / S / W slots ...\n"
        "    ImNodal::EndNode();\n"
        "}\n"
        "\n"
        "// Manhattan link between two diamonds (flowchart style) :\n"
        "if (ImNodal::BeginLink(linkId, eastOfA, westOfB)) {\n"
        "    ImVec2 p0 = ImNodal::GetLinkFromPos();\n"
        "    ImVec2 p1 = ImNodal::GetLinkToPos();\n"
        "    const float midX = (p0.x + p1.x) * 0.5f;\n"
        "    ImVec2 pts[4] = { p0, ImVec2(midX, p0.y), ImVec2(midX, p1.y), p1 };\n"
        "    ImNodal::LinkPolyline(pts, 4);\n"
        "    ImNodal::EndLink();\n"
        "}\n");
}

// =====================================================================
// Section : Navigation
// =====================================================================
static void s_sectionNavigation() {
    ImGui::TextWrapped(
        "NavigateToContent fits the view to all nodes. NavigateToSelection "
        "fits to the current selection (falls back to all-content when "
        "nothing is selected). Both are called outside of any Begin/End — "
        "ImNodal targets the lastTouched canvas.");

    static ImVec2 s_pos[6] = {
        ImVec2(-400.0f, -200.0f),
        ImVec2(-200.0f, -100.0f),
        ImVec2(   0.0f,    0.0f),
        ImVec2( 200.0f,  100.0f),
        ImVec2( 400.0f,  200.0f),
        ImVec2(-100.0f,  200.0f),
    };

    const bool hasCanvas = ImNodal::HasEditor("##canvas_nav", 1);
    if (ImGui::Button("Navigate to content") && hasCanvas) {
        ImNodal::SetCurrentEditor("##canvas_nav", 1);
        ImNodal::NavigateToContent();
    }
    ImGui::SameLine();
    if (ImGui::Button("Navigate to selection") && hasCanvas) {
        ImNodal::SetCurrentEditor("##canvas_nav", 1);
        ImNodal::NavigateToSelection();
    }
    ImGui::SameLine();
    s_helpMarker("Click a node first, then 'Navigate to selection' fits the view on it. "
                 "With nothing selected, both buttons behave the same.");

    if (ImNodal::BeginCanvas("##canvas_nav", ImVec2(0.0f, 240.0f))) {
        if (ImNodal::BeginGraph(1)) {
            for (int i = 0; i < 6; ++i) {
                if (ImNodal::BeginNode((Id)(i + 1), &s_pos[i])) {
                    ImGui::Text("Node %d", i + 1);
                    ImNodal::EndNode();
                }
            }
            ImNodal::EndGraph();
        }
        ImNodal::EndCanvas();
    }

    s_showCode("Show code",
        "if (ImGui::Button(\"Fit content\"))   ImNodal::NavigateToContent();\n"
        "if (ImGui::Button(\"Fit selection\")) ImNodal::NavigateToSelection();\n"
        "// Both use the canvas widget's last-frame size to compute the zoom.\n");
}

// =====================================================================
// Section : Style editor
// =====================================================================
static void s_sectionStyleEditor() {
    ImGui::TextWrapped(
        "The two editor widgets — also reachable from the menu bar above — "
        "mutate ImNodal::GetStyle() in place. Every section in this demo "
        "picks up the change immediately on the next frame.");
    if (ImGui::BeginTabBar("##style_tabs")) {
        if (ImGui::BeginTabItem("Colors")) {
            ImNodal::ShowStyleColorsEditor();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Vars")) {
            ImNodal::ShowStyleVarsEditor();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// =====================================================================
// Menu bar
// =====================================================================
static void s_menuBar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("Style")) {
        if (ImGui::BeginMenu("Colors")) {
            ImNodal::ShowStyleColorsEditor();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Vars")) {
            ImNodal::ShowStyleVarsEditor();
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::TextDisabled("ImNodal v" IMNODAL_VERSION);
        ImGui::Separator();
        ImGui::TextUnformatted("Each section below ships a live canvas\n"
                               "exercising one feature, plus the source\n"
                               "code you'd copy-paste into your host.");
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

}  // namespace

// =====================================================================
// Public entry point
// =====================================================================

IMNODAL_API void ShowDemoWindow(bool* apoOpen) {
    if (apoOpen != nullptr && !(*apoOpen)) return;

    const ImGuiViewport* pMainViewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(pMainViewport->WorkPos.x + 650.0f, pMainViewport->WorkPos.y + 20.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(620.0f, 720.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("ImNodal Demo", apoOpen, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }

    s_menuBar();

    ImGui::TextWrapped(
        "Guided tour of every public ImNodal feature. Each section opens a "
        "live, isolated canvas + tweakable widgets and ships the matching "
        "source snippet you can copy-paste into your host.");
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Canvas basics"))                                  s_sectionCanvasBasics();
    if (ImGui::CollapsingHeader("Graph & multi-graph"))                            s_sectionGraphMultiGraph();
    if (ImGui::CollapsingHeader("Nodes & Slots"))                                  s_sectionNodesSlots();
    if (ImGui::CollapsingHeader("Layout primitives"))                              s_sectionLayoutPrimitives();
    if (ImGui::CollapsingHeader("Links — Bezier / Line / Polyline / Manhattan"))   s_sectionLinks();
    if (ImGui::CollapsingHeader("Flow animation"))                                 s_sectionFlowAnimation();
    if (ImGui::CollapsingHeader("Connection Create"))                              s_sectionConnectionCreate();
    if (ImGui::CollapsingHeader("Selection"))                                      s_sectionSelection();
    if (ImGui::CollapsingHeader("Delete & Shortcuts"))                             s_sectionDeleteShortcuts();
    if (ImGui::CollapsingHeader("MiniMap"))                                        s_sectionMiniMap();
    if (ImGui::CollapsingHeader("Custom Hitbox — flowchart diamond"))              s_sectionDiamond();
    if (ImGui::CollapsingHeader("Navigation"))                                     s_sectionNavigation();
    if (ImGui::CollapsingHeader("Style editor"))                                   s_sectionStyleEditor();

    ImGui::End();
}

}  // namespace ImNodal
