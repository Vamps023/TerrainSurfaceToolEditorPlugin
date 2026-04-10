#include "TerrainSurfaceToolEditorPlugin.h"

#include "LandscapeSaveManager.h"
#include "SurfaceRasterizer.h"
#include "TerrainManipulator.h"
#include "TerrainToolPanel.h"

#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>
#include <editor/UnigineWindowManager.h>

#include <QtWidgets/QApplication>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenuBar>

#include <UnigineLog.h>
#include <UnigineNodes.h>
#include <UnigineWorld.h>

using namespace Unigine;

namespace UnigineEditor
{
TerrainSurfaceToolEditorPlugin::TerrainSurfaceToolEditorPlugin() = default;
TerrainSurfaceToolEditorPlugin::~TerrainSurfaceToolEditorPlugin() = default;

bool TerrainSurfaceToolEditorPlugin::init()
{
    try
    {
        save_manager_ = std::make_unique<LandscapeSaveManager>(false);
        terrain_manipulator_ = std::make_unique<TerrainManipulator>(*save_manager_);
        terrain_tool_panel_ = std::make_unique<TerrainToolPanel>(this);

        setupMenu();
        WindowManager::add(terrain_tool_panel_.get(), WindowManager::AreaType::ROOT_AREA_RIGHT);
        return true;
    }
    catch (const std::exception& exception)
    {
        Log::error("[TerrainSurfaceTool] Failed to initialize plugin: %s\n", exception.what());
        return false;
    }
}

void TerrainSurfaceToolEditorPlugin::shutdown()
{
    if (terrain_manipulator_)
        terrain_manipulator_->flushPendingSaves();

    if (vamps_menu_ && terrain_tool_action_)
        vamps_menu_->removeAction(terrain_tool_action_);

    if (terrain_tool_action_)
    {
        delete terrain_tool_action_;
        terrain_tool_action_ = nullptr;
    }

    if (terrain_tool_panel_)
        WindowManager::remove(terrain_tool_panel_.get());

    terrain_tool_panel_.reset();
    terrain_manipulator_.reset();
    save_manager_.reset();

    if (owns_menu_ && vamps_menu_)
    {
        delete vamps_menu_;
        vamps_menu_ = nullptr;
        owns_menu_ = false;
    }
}

std::vector<NodePtr> TerrainSurfaceToolEditorPlugin::getSelectedMeshNodes() const
{
    std::vector<NodePtr> root_nodes;
    const SelectorNodes* selector_nodes = Selection::getSelectorNodes();
    if (!selector_nodes)
        return root_nodes;

    const Vector<NodePtr>& selected_nodes = selector_nodes->getNodes();
    root_nodes.reserve(selected_nodes.size());
    for (const auto& node : selected_nodes)
        root_nodes.push_back(node);

    return SurfaceRasterizer::collectMeshNodesRecursive(root_nodes);
}

std::vector<ObjectLandscapeTerrainPtr> TerrainSurfaceToolEditorPlugin::getLandscapeTerrains() const
{
    std::vector<ObjectLandscapeTerrainPtr> terrains;
    Vector<NodePtr> root_nodes;
    World::getRootNodes(root_nodes);

    std::unordered_set<int> visited_node_ids;
    terrains.reserve(root_nodes.size());
    for (const auto& root : root_nodes)
        collectLandscapeTerrainsRecursive(root, terrains, visited_node_ids);

    return terrains;
}

ObjectLandscapeTerrainPtr TerrainSurfaceToolEditorPlugin::getLandscapeTerrainById(int node_id) const
{
    if (node_id < 0)
        return Landscape::getActiveTerrain();

    auto node = Node::getNode(node_id);
    return checked_ptr_cast<ObjectLandscapeTerrain>(node);
}

std::vector<LandscapeLayerMapPtr> TerrainSurfaceToolEditorPlugin::getLandscapeLayerMaps(
    const ObjectLandscapeTerrainPtr& terrain) const
{
    std::vector<LandscapeLayerMapPtr> layer_maps;
    if (!terrain)
        return layer_maps;

    std::unordered_set<int> visited_node_ids;
    collectLandscapeLayerMapsRecursive(terrain, layer_maps, visited_node_ids);

    return layer_maps;
}

LandscapeLayerMapPtr TerrainSurfaceToolEditorPlugin::getLandscapeLayerMapById(int node_id) const
{
    if (node_id < 0)
        return nullptr;

    auto node = Node::getNode(node_id);
    return checked_ptr_cast<LandscapeLayerMap>(node);
}

void TerrainSurfaceToolEditorPlugin::setupMenu()
{
    vamps_menu_ = WindowManager::findMenu("VampsPlugin");
    if (!vamps_menu_)
    {
        if (auto* main_window = qobject_cast<QMainWindow*>(QApplication::activeWindow()))
        {
            vamps_menu_ = new QMenu("VampsPlugin", main_window);
            if (main_window->menuBar())
                main_window->menuBar()->addMenu(vamps_menu_);
            owns_menu_ = true;
        }
        else
        {
            vamps_menu_ = new QMenu("VampsPlugin");
            owns_menu_ = true;
        }
    }

    terrain_tool_action_ = new QAction("Terrain Surface Tool", vamps_menu_);
    connect(terrain_tool_action_, &QAction::triggered,
            this, &TerrainSurfaceToolEditorPlugin::openTerrainTool);

    if (vamps_menu_)
        vamps_menu_->addAction(terrain_tool_action_);
}

void TerrainSurfaceToolEditorPlugin::collectLandscapeTerrainsRecursive(
    const NodePtr& node,
    std::vector<ObjectLandscapeTerrainPtr>& out_terrains,
    std::unordered_set<int>& visited_node_ids)
{
    if (!node)
        return;

    const int node_id = node->getID();
    if (!visited_node_ids.insert(node_id).second)
        return;

    if (node->getType() == Node::OBJECT_LANDSCAPE_TERRAIN)
    {
        auto terrain = checked_ptr_cast<ObjectLandscapeTerrain>(node);
        if (terrain)
            out_terrains.push_back(terrain);
    }
    else if (node->getType() == Node::NODE_REFERENCE)
    {
        auto reference = checked_ptr_cast<NodeReference>(node);
        if (reference)
        {
            auto target = reference->getReference();
            if (target)
                collectLandscapeTerrainsRecursive(target, out_terrains, visited_node_ids);
        }
    }

    for (int child_index = 0; child_index < node->getNumChildren(); ++child_index)
        collectLandscapeTerrainsRecursive(node->getChild(child_index), out_terrains, visited_node_ids);
}

void TerrainSurfaceToolEditorPlugin::collectLandscapeLayerMapsRecursive(
    const NodePtr& node,
    std::vector<LandscapeLayerMapPtr>& out_layer_maps,
    std::unordered_set<int>& visited_node_ids)
{
    if (!node)
        return;

    const int node_id = node->getID();
    if (!visited_node_ids.insert(node_id).second)
        return;

    if (node->getType() == Node::LANDSCAPE_LAYER_MAP)
    {
        auto layer_map = checked_ptr_cast<LandscapeLayerMap>(node);
        if (layer_map)
            out_layer_maps.push_back(layer_map);
    }
    else if (node->getType() == Node::NODE_REFERENCE)
    {
        auto reference = checked_ptr_cast<NodeReference>(node);
        if (reference)
        {
            auto target = reference->getReference();
            if (target)
                collectLandscapeLayerMapsRecursive(target, out_layer_maps, visited_node_ids);
        }
    }

    for (int child_index = 0; child_index < node->getNumChildren(); ++child_index)
        collectLandscapeLayerMapsRecursive(node->getChild(child_index), out_layer_maps, visited_node_ids);
}

void TerrainSurfaceToolEditorPlugin::openTerrainTool()
{
    if (!terrain_tool_panel_)
        return;

    WindowManager::show(terrain_tool_panel_.get());
    WindowManager::activate(terrain_tool_panel_.get());
}
}
