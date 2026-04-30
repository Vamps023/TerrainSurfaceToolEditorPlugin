#pragma once

#include <UnigineNode.h>
#include <UnigineNodes.h>

#include <unordered_set>
#include <vector>

class NodeTreeWalker
{
public:
    template<int TargetNodeType, typename CastType>
    static void collectNodesRecursive(const Unigine::NodePtr& node,
                                      std::vector<Unigine::Ptr<CastType>>& outNodes,
                                      std::unordered_set<int>& visitedIds)
    {
        if (!node)
            return;

        const int nodeId = node->getID();
        if (!visitedIds.insert(nodeId).second)
            return;

        if (node->getType() == TargetNodeType)
        {
            auto cast = Unigine::checked_ptr_cast<CastType>(node);
            if (cast)
                outNodes.push_back(cast);
        }
        else if (node->getType() == Unigine::Node::NODE_REFERENCE)
        {
            auto reference = Unigine::checked_ptr_cast<Unigine::NodeReference>(node);
            if (reference)
            {
                auto target = reference->getReference();
                if (target)
                    collectNodesRecursive<TargetNodeType, CastType>(target, outNodes, visitedIds);
            }
        }

        for (int i = 0; i < node->getNumChildren(); ++i)
            collectNodesRecursive<TargetNodeType, CastType>(node->getChild(i), outNodes, visitedIds);
    }

    static void collectMeshNodesRecursive(const Unigine::NodePtr& node,
                                          std::vector<Unigine::NodePtr>& outNodes,
                                          std::unordered_set<int>& visitedIds,
                                          std::unordered_set<int>& collectedMeshIds)
    {
        if (!node)
            return;

        const int nodeId = node->getID();
        if (!visitedIds.insert(nodeId).second)
            return;

        const int type = node->getType();
        if (type == Unigine::Node::OBJECT_MESH_STATIC || type == Unigine::Node::OBJECT_MESH_DYNAMIC)
        {
            if (collectedMeshIds.insert(nodeId).second)
                outNodes.push_back(node);
        }
        else if (type == Unigine::Node::NODE_REFERENCE)
        {
            auto reference = Unigine::checked_ptr_cast<Unigine::NodeReference>(node);
            if (reference)
            {
                auto target = reference->getReference();
                if (target)
                    collectMeshNodesRecursive(target, outNodes, visitedIds, collectedMeshIds);
            }
        }

        for (int i = 0; i < node->getNumChildren(); ++i)
            collectMeshNodesRecursive(node->getChild(i), outNodes, visitedIds, collectedMeshIds);
    }
};
