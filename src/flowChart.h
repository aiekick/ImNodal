#pragma once

#include <ImNodal.h>

// https://en.wikipedia.org/wiki/Flowchart
//
// FlowChart hosts a single ImNodal canvas+graph and exercises the lib by
// rendering classic flowchart shapes (start/end, process, decision, I/O,
// predefined process) wired by Manhattan links with arrow heads. Data-
// oriented : nodes/links live in std::vector<FlowNode>/<FlowLink>, drawn
// inline via a switch on FlowNodeKind. No helper soup, no sub-classes.
class FlowChart {
public:
    void display();

private:
    void m_drawConditionalNode(const char* apLabel, const ImNodal::Id aNodeId);
};
