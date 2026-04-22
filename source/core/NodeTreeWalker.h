#pragma once

#include <UnigineNode.h>
#include <UnigineNodes.h>

#include <unordered_set>
#include <vector>

namespace NodeTreeWalker
{

template<int TargetNodeType, typename CastType>
inline void collectNodesRecursive(const Unigine::NodePtr& node,
                                  std::vector<Unigine::Ptr<CastType>>& out_nodes,
                                  std::unordered_set<int>& visited_ids)
{
    if (!node)
        return;

    const int node_id = node->getID();
    if (!visited_ids.insert(node_id).second)
        return;

    if (node->getType() == TargetNodeType)
    {
        auto cast = Unigine::checked_ptr_cast<CastType>(node);
        if (cast)
            out_nodes.push_back(cast);
    }
    else if (node->getType() == Unigine::Node::NODE_REFERENCE)
    {
        auto reference = Unigine::checked_ptr_cast<Unigine::NodeReference>(node);
        if (reference)
        {
            auto target = reference->getReference();
            if (target)
                collectNodesRecursive<TargetNodeType, CastType>(target, out_nodes, visited_ids);
        }
    }

    for (int i = 0; i < node->getNumChildren(); ++i)
        collectNodesRecursive<TargetNodeType, CastType>(node->getChild(i), out_nodes, visited_ids);
}

inline void collectMeshNodesRecursive(const Unigine::NodePtr& node,
                                      std::vector<Unigine::NodePtr>& out_nodes,
                                      std::unordered_set<int>& visited_ids,
                                      std::unordered_set<int>& collected_mesh_ids)
{
    if (!node)
        return;

    const int node_id = node->getID();
    if (!visited_ids.insert(node_id).second)
        return;

    const int type = node->getType();
    if (type == Unigine::Node::OBJECT_MESH_STATIC || type == Unigine::Node::OBJECT_MESH_DYNAMIC)
    {
        if (collected_mesh_ids.insert(node_id).second)
            out_nodes.push_back(node);
    }
    else if (type == Unigine::Node::NODE_REFERENCE)
    {
        auto reference = Unigine::checked_ptr_cast<Unigine::NodeReference>(node);
        if (reference)
        {
            auto target = reference->getReference();
            if (target)
                collectMeshNodesRecursive(target, out_nodes, visited_ids, collected_mesh_ids);
        }
    }

    for (int i = 0; i < node->getNumChildren(); ++i)
        collectMeshNodesRecursive(node->getChild(i), out_nodes, visited_ids, collected_mesh_ids);
}

}
