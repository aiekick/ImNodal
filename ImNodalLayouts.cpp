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

#include "ImNodalLayouts.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ImNodal {

bool HierarchicalLayout::Apply(LayoutGraph& aGraph) {
    const size_t nodeCount = aGraph.nodes.size();
    for (size_t k = 0U; k < aGraph.edges.size(); ++k) {
        aGraph.edges[k].excluded = false;  // back-edge classification rewrites this below
    }
    if (nodeCount == 0U) {
        return false;
    }

    // layout spacing (customizable, see HierarchicalLayout::Settings)
    const float columnGap = m_settings.columnGap;    // horizontal gap between two layers
    const float rowGap = m_settings.rowGap;          // vertical gap between two nodes of a layer
    const float clusterGap = m_settings.clusterGap;  // extra vertical gap between two different clusters (bands)

    // scan each node's connections through the edges : being an edge dst means an INPUT is
    // connected, being an edge src means an OUTPUT is connected.
    struct FlowEdge {
        size_t src{0};
        size_t dst{0};
        size_t edgeIndex{0};
    };
    std::vector<FlowEdge> edges;
    std::vector<bool> hasInputConn(nodeCount, false);
    std::vector<bool> hasOutputConn(nodeCount, false);
    for (size_t k = 0U; k < aGraph.edges.size(); ++k) {
        const size_t srcIndex = static_cast<size_t>(aGraph.edges[k].fromNode);
        const size_t dstIndex = static_cast<size_t>(aGraph.edges[k].toNode);
        if (srcIndex < nodeCount && dstIndex < nodeCount && srcIndex != dstIndex) {
            FlowEdge edge;
            edge.src = srcIndex;
            edge.dst = dstIndex;
            edge.edgeIndex = k;
            edges.push_back(edge);
            hasOutputConn[srcIndex] = true;
            hasInputConn[dstIndex] = true;
        }
    }

    // back-edge classification (iterative DFS) : an edge whose target is still on the current DFS
    // path closes a cycle -> excluded from the hierarchy and flagged for orthogonal drawing.
    std::vector<std::vector<size_t>> outEdges(nodeCount);
    for (size_t e = 0U; e < edges.size(); ++e) {
        outEdges[edges[e].src].push_back(e);
    }
    std::vector<int32_t> color(nodeCount, 0);  // 0 = white, 1 = gray (on path), 2 = black (done)
    std::vector<bool> isBack(edges.size(), false);
    for (size_t root = 0U; root < nodeCount; ++root) {
        if (color[root] != 0) {
            continue;
        }
        std::vector<std::pair<size_t, size_t>> stack;
        stack.push_back(std::make_pair(root, static_cast<size_t>(0)));
        color[root] = 1;
        while (!stack.empty()) {
            const size_t node = stack.back().first;
            if (stack.back().second < outEdges[node].size()) {
                const size_t e = outEdges[node][stack.back().second];
                stack.back().second += 1;
                const size_t target = edges[e].dst;
                if (color[target] == 0) {
                    color[target] = 1;
                    stack.push_back(std::make_pair(target, static_cast<size_t>(0)));
                } else if (color[target] == 1) {
                    isBack[e] = true;
                }
            } else {
                color[node] = 2;
                stack.pop_back();
            }
        }
    }
    for (size_t e = 0U; e < edges.size(); ++e) {
        if (isBack[e]) {
            aGraph.edges[edges[e].edgeIndex].excluded = true;
        }
    }

    // clean DAG (non-back edges) : successors / predecessors / in-degree, plus the slot index used
    // ON THE NEIGHBOUR (its output slot for a predecessor edge, its input slot for a successor edge),
    // so the ordering can sort siblings by slot height and cut cross-column crossings.
    std::vector<std::vector<size_t>> succ(nodeCount);
    std::vector<std::vector<size_t>> pred(nodeCount);
    std::vector<std::vector<int32_t>> succSlot(nodeCount);
    std::vector<std::vector<int32_t>> predSlot(nodeCount);
    std::vector<int32_t> inDegree(nodeCount, 0);
    int32_t maxSlot = 0;
    for (size_t e = 0U; e < edges.size(); ++e) {
        if (isBack[e]) {
            continue;
        }
        const int32_t srcSlot = aGraph.edges[edges[e].edgeIndex].fromSlot;
        const int32_t dstSlot = aGraph.edges[edges[e].edgeIndex].toSlot;
        succ[edges[e].src].push_back(edges[e].dst);
        succSlot[edges[e].src].push_back(dstSlot);  // slot on the successor = its input slot
        pred[edges[e].dst].push_back(edges[e].src);
        predSlot[edges[e].dst].push_back(srcSlot);  // slot on the predecessor = its output slot
        inDegree[edges[e].dst] += 1;
        maxSlot = std::max(maxSlot, std::max(srcSlot, dstSlot));
    }
    const float slotScale = 1.0f / static_cast<float>(maxSlot + 2);  // keeps the slot fraction < 1

    // ASAP longest-path layering (Kahn) : follow the links left -> right
    std::vector<int32_t> layer(nodeCount, 0);
    std::vector<int32_t> remaining = inDegree;
    std::vector<size_t> ready;
    for (size_t i = 0U; i < nodeCount; ++i) {
        if (remaining[i] == 0) {
            ready.push_back(i);
        }
    }
    for (size_t head = 0U; head < ready.size(); ++head) {
        const size_t node = ready[head];
        for (const size_t target : succ[node]) {
            if (layer[target] < layer[node] + 1) {
                layer[target] = layer[node] + 1;
            }
            if (--remaining[target] == 0) {
                ready.push_back(target);
            }
        }
    }
    int32_t maxLayer = 0;
    for (size_t i = 0U; i < nodeCount; ++i) {
        maxLayer = std::max(maxLayer, layer[i]);
    }

    // classification overrides :
    //  - sink (input connected, output connected to nothing) -> forced to the last column (right)
    for (size_t i = 0U; i < nodeCount; ++i) {
        if (hasInputConn[i] && !hasOutputConn[i]) {
            layer[i] = maxLayer;
        }
    }
    //  - output-only (input connected to nothing, output connected) -> one column before its consumer
    for (size_t i = 0U; i < nodeCount; ++i) {
        if (!hasInputConn[i] && hasOutputConn[i] && !succ[i].empty()) {
            int32_t earliest = maxLayer;
            for (const size_t target : succ[i]) {
                earliest = std::min(earliest, layer[target]);
            }
            layer[i] = std::max(0, earliest - 1);
        }
    }

    // group nodes by column
    std::vector<std::vector<size_t>> layers(static_cast<size_t>(maxLayer) + 1U);
    for (size_t i = 0U; i < nodeCount; ++i) {
        layers[static_cast<size_t>(layer[i])].push_back(i);
    }

    // clusters = connected components of the graph (all edges, incl. back-edges). Each component
    // becomes its own horizontal BAND, stacked with extra spacing, so connected groups read as
    // distinct clusters. Components are disconnected, so their relative order is free.
    std::vector<size_t> clusterParent(nodeCount);
    for (size_t i = 0U; i < nodeCount; ++i) {
        clusterParent[i] = i;
    }
    const auto findRoot = [&clusterParent](size_t aNode) -> size_t {
        while (clusterParent[aNode] != aNode) {
            clusterParent[aNode] = clusterParent[clusterParent[aNode]];  // path halving
            aNode = clusterParent[aNode];
        }
        return aNode;
    };
    for (size_t e = 0U; e < edges.size(); ++e) {
        const size_t rootSrc = findRoot(edges[e].src);
        const size_t rootDst = findRoot(edges[e].dst);
        if (rootSrc != rootDst) {
            clusterParent[rootSrc] = rootDst;
        }
    }
    std::vector<int32_t> clusterSize(nodeCount, 0);
    for (size_t i = 0U; i < nodeCount; ++i) {
        clusterSize[findRoot(i)] += 1;
    }
    std::vector<size_t> clusterRoots;
    for (size_t i = 0U; i < nodeCount; ++i) {
        if (findRoot(i) == i) {
            clusterRoots.push_back(i);
        }
    }
    std::sort(clusterRoots.begin(), clusterRoots.end(), [&clusterSize](size_t aLeft, size_t aRight) {
        if (clusterSize[aLeft] != clusterSize[aRight]) {
            return clusterSize[aLeft] > clusterSize[aRight];  // biggest cluster band on top
        }
        return aLeft < aRight;
    });
    std::vector<int32_t> rankOfRoot(nodeCount, 0);
    for (size_t r = 0U; r < clusterRoots.size(); ++r) {
        rankOfRoot[clusterRoots[r]] = static_cast<int32_t>(r);
    }
    std::vector<int32_t> clusterRankOf(nodeCount, 0);
    for (size_t i = 0U; i < nodeCount; ++i) {
        clusterRankOf[i] = rankOfRoot[findRoot(i)];
    }

    // vertical ordering : barycenter over all flow neighbours (a few sweeps) so connected nodes
    // sit next to each other and links cross less -> readable clusters.
    std::vector<int32_t> orderInLayer(nodeCount, 0);
    for (size_t layerIndex = 0U; layerIndex < layers.size(); ++layerIndex) {
        for (size_t slot = 0U; slot < layers[layerIndex].size(); ++slot) {
            orderInLayer[layers[layerIndex][slot]] = static_cast<int32_t>(slot);
        }
    }
    // neighbours weighted by 1 / columnDistance^2 : close links dominate -> tight clusters,
    // far links barely influence the order.
    const auto neighborBary = [&succ, &pred, &succSlot, &predSlot, &orderInLayer, &layer, slotScale](size_t aNode) -> float {
        float sum = 0.0f;
        float weightSum = 0.0f;
        for (size_t i = 0U; i < succ[aNode].size(); ++i) {
            const size_t target = succ[aNode][i];
            const int32_t diff = layer[aNode] - layer[target];
            const int32_t dist = std::max(1, diff < 0 ? -diff : diff);
            const float w = 1.0f / static_cast<float>(dist * dist);
            // neighbour order refined by the slot height on that neighbour (fraction < 1)
            sum += (static_cast<float>(orderInLayer[target]) + static_cast<float>(succSlot[aNode][i]) * slotScale) * w;
            weightSum += w;
        }
        for (size_t i = 0U; i < pred[aNode].size(); ++i) {
            const size_t source = pred[aNode][i];
            const int32_t diff = layer[aNode] - layer[source];
            const int32_t dist = std::max(1, diff < 0 ? -diff : diff);
            const float w = 1.0f / static_cast<float>(dist * dist);
            sum += (static_cast<float>(orderInLayer[source]) + static_cast<float>(predSlot[aNode][i]) * slotScale) * w;
            weightSum += w;
        }
        if (weightSum == 0.0f) {
            return static_cast<float>(orderInLayer[aNode]);
        }
        return sum / weightSum;
    };
    for (int32_t sweep = 0; sweep < 4; ++sweep) {
        for (size_t layerIndex = 0U; layerIndex < layers.size(); ++layerIndex) {
            std::sort(layers[layerIndex].begin(), layers[layerIndex].end(),  //
                [&neighborBary, &clusterRankOf](size_t aLeft, size_t aRight) {
                    if (clusterRankOf[aLeft] != clusterRankOf[aRight]) {
                        return clusterRankOf[aLeft] < clusterRankOf[aRight];  // keep clusters grouped as bands
                    }
                    return neighborBary(aLeft) < neighborBary(aRight);
                });
            for (size_t slot = 0U; slot < layers[layerIndex].size(); ++slot) {
                orderInLayer[layers[layerIndex][slot]] = static_cast<int32_t>(slot);
            }
        }
    }

    // X per column : max node width of the column + gap
    std::vector<float> columnX(layers.size(), 0.0f);
    {
        float x = 0.0f;
        for (size_t layerIndex = 0U; layerIndex < layers.size(); ++layerIndex) {
            columnX[layerIndex] = x;
            float maxWidth = 0.0f;
            for (const size_t i : layers[layerIndex]) {
                maxWidth = std::max(maxWidth, aGraph.nodes[i].size.x);
            }
            x += maxWidth + columnGap;
        }
    }

    // Y assignment : pull each node onto the average height of its neighbours so connected nodes sit
    // together (short links, readable clusters), instead of centering every column on zero. Each
    // sweep computes a target center per node, packs the column in order with the min gap, then
    // re-centers the column on the target mean to avoid a downward drift.
    std::vector<float> nodeY(nodeCount, 0.0f);
    for (size_t layerIndex = 0U; layerIndex < layers.size(); ++layerIndex) {
        float y = 0.0f;
        for (const size_t i : layers[layerIndex]) {
            nodeY[i] = y;
            y += aGraph.nodes[i].size.y + rowGap;
        }
    }
    const int32_t coordSweeps = 40;
    for (int32_t sweep = 0; sweep < coordSweeps; ++sweep) {
        const bool leftToRight = (sweep % 2 == 0);
        for (size_t step = 0U; step < layers.size(); ++step) {
            const size_t layerIndex = leftToRight ? step : (layers.size() - 1U - step);
            const auto& column = layers[layerIndex];
            if (column.empty()) {
                continue;
            }
            std::vector<float> desiredCenter(column.size(), 0.0f);
            float sumDesired = 0.0f;
            for (size_t slot = 0U; slot < column.size(); ++slot) {
                const size_t node = column[slot];
                // forward sweep centers each node on its PREDECESSORS (the previous column), backward
                // sweep on its successors ; fall back to the other side when this one is empty.
                const std::vector<size_t>& primary = leftToRight ? pred[node] : succ[node];
                const std::vector<size_t>& fallback = leftToRight ? succ[node] : pred[node];
                const std::vector<size_t>& neighbours = !primary.empty() ? primary : fallback;
                float sum = 0.0f;
                float weightSum = 0.0f;
                for (const size_t other : neighbours) {
                    const int32_t diff = layer[node] - layer[other];
                    const int32_t dist = std::max(1, diff < 0 ? -diff : diff);
                    const float w = 1.0f / static_cast<float>(dist * dist);  // close links dominate
                    sum += (nodeY[other] + aGraph.nodes[other].size.y * 0.5f) * w;
                    weightSum += w;
                }
                const float current = nodeY[node] + aGraph.nodes[node].size.y * 0.5f;
                desiredCenter[slot] = (weightSum == 0.0f) ? current : (sum / weightSum);
                sumDesired += desiredCenter[slot];
            }
            // pack the column in order, each node near its desired center ; a bigger gap separates
            // two different clusters so the bands stay visually distinct.
            float prevBottom = -1.0e9f;
            int32_t prevCluster = -1;
            for (size_t slot = 0U; slot < column.size(); ++slot) {
                const size_t node = column[slot];
                const float gap = (prevCluster >= 0 && clusterRankOf[node] != prevCluster) ? clusterGap : rowGap;
                float top = desiredCenter[slot] - aGraph.nodes[node].size.y * 0.5f;
                if (top < prevBottom + gap) {
                    top = prevBottom + gap;
                }
                nodeY[node] = top;
                prevBottom = top + aGraph.nodes[node].size.y;
                prevCluster = clusterRankOf[node];
            }
            // re-center the column on the desired mean (kills the downward drift from packing)
            float sumActual = 0.0f;
            for (const size_t node : column) {
                sumActual += nodeY[node] + aGraph.nodes[node].size.y * 0.5f;
            }
            const float shift = (sumDesired - sumActual) / static_cast<float>(column.size());
            for (const size_t node : column) {
                nodeY[node] += shift;
            }
        }
        // global detrend : remove the vertical offset AND the slope-vs-column. Without this the
        // per-column alignment settles into a stable DIAGONAL (a uniform tilt is a fixed point of
        // the median alignment, so it never flattens on its own).
        float sumX = 0.0f;
        float sumY = 0.0f;
        size_t globalCount = 0U;
        for (size_t i = 0U; i < nodeCount; ++i) {
            sumX += columnX[static_cast<size_t>(layer[i])];
            sumY += nodeY[i] + aGraph.nodes[i].size.y * 0.5f;
            ++globalCount;
        }
        if (globalCount > 0U) {
            const float meanX = sumX / static_cast<float>(globalCount);
            const float meanY = sumY / static_cast<float>(globalCount);
            float covXY = 0.0f;
            float varX = 0.0f;
            for (size_t i = 0U; i < nodeCount; ++i) {
                const float dx = columnX[static_cast<size_t>(layer[i])] - meanX;
                const float dy = (nodeY[i] + aGraph.nodes[i].size.y * 0.5f) - meanY;
                covXY += dx * dy;
                varX += dx * dx;
            }
            const float slope = (varX > 1.0e-6f) ? (covXY / varX) : 0.0f;
            for (size_t i = 0U; i < nodeCount; ++i) {
                const float dx = columnX[static_cast<size_t>(layer[i])] - meanX;
                nodeY[i] -= meanY + slope * dx;
            }
        }
    }

    bool placedAny = false;
    for (size_t layerIndex = 0U; layerIndex < layers.size(); ++layerIndex) {
        for (const size_t i : layers[layerIndex]) {
            aGraph.nodes[i].pos = ImVec2(columnX[layerIndex], nodeY[i]);
            placedAny = true;
        }
    }
    return placedAny;
}

}  // namespace ImNodal
