#include "TerrainSurfaceToolEditorPlugin.h"

#include "NodeTreeWalker.h"
#include "../landscape/LandscapeSaveManager.h"
#include "../rasterizer/SurfaceRasterizer.h"
#include "../terrain/TerrainManipulator.h"
#include "../ui/TerrainToolPanel.h"

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
        saveManager = std::make_unique<LandscapeSaveManager>(false);
        terrainManipulator = std::make_unique<TerrainManipulator>(*saveManager);
        terrainToolPanel = std::make_unique<TerrainToolPanel>(this);

        setupMenu();
        WindowManager::add(terrainToolPanel.get(), WindowManager::AreaType::ROOT_AREA_RIGHT);
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
    if (terrainManipulator)
        terrainManipulator->flushPendingSaves();

    if (pluginMenu && terrainToolAction)
        pluginMenu->removeAction(terrainToolAction);

    if (terrainToolAction)
    {
        delete terrainToolAction;
        terrainToolAction = nullptr;
    }

    if (terrainToolPanel)
        WindowManager::remove(terrainToolPanel.get());

    terrainToolPanel.reset();
    terrainManipulator.reset();
    saveManager.reset();

    if (ownsMenu && pluginMenu)
    {
        delete pluginMenu;
        pluginMenu = nullptr;
        ownsMenu = false;
    }
}

std::vector<NodePtr> TerrainSurfaceToolEditorPlugin::getSelectedMeshNodes() const
{
    std::vector<NodePtr> rootNodes;
    const SelectorNodes* selectorNodes = Selection::getSelectorNodes();
    if (!selectorNodes)
        return rootNodes;

    const Vector<NodePtr>& selectedNodes = selectorNodes->getNodes();
    rootNodes.reserve(selectedNodes.size());
    for (const auto& node : selectedNodes)
        rootNodes.push_back(node);

    return SurfaceRasterizer::collectMeshNodesRecursive(rootNodes);
}

std::vector<ObjectLandscapeTerrainPtr> TerrainSurfaceToolEditorPlugin::getLandscapeTerrains() const
{
    std::vector<ObjectLandscapeTerrainPtr> terrains;
    Vector<NodePtr> rootNodes;
    World::getRootNodes(rootNodes);

    std::unordered_set<int> visitedNodeIds;
    terrains.reserve(rootNodes.size());
    for (const auto& root : rootNodes)
        NodeTreeWalker::collectNodesRecursive<Node::OBJECT_LANDSCAPE_TERRAIN, ObjectLandscapeTerrain>(root, terrains, visitedNodeIds);

    return terrains;
}

ObjectLandscapeTerrainPtr TerrainSurfaceToolEditorPlugin::getLandscapeTerrainById(int nodeId) const
{
    if (nodeId < 0)
        return Landscape::getActiveTerrain();

    auto node = Node::getNode(nodeId);
    return checked_ptr_cast<ObjectLandscapeTerrain>(node);
}

std::vector<LandscapeLayerMapPtr> TerrainSurfaceToolEditorPlugin::getLandscapeLayerMaps(
    const ObjectLandscapeTerrainPtr& terrain) const
{
    std::vector<LandscapeLayerMapPtr> layerMaps;
    if (!terrain)
        return layerMaps;

    std::unordered_set<int> visitedNodeIds;
    NodeTreeWalker::collectNodesRecursive<Node::LANDSCAPE_LAYER_MAP, LandscapeLayerMap>(terrain, layerMaps, visitedNodeIds);

    return layerMaps;
}

LandscapeLayerMapPtr TerrainSurfaceToolEditorPlugin::getLandscapeLayerMapById(int nodeId) const
{
    if (nodeId < 0)
        return nullptr;

    auto node = Node::getNode(nodeId);
    return checked_ptr_cast<LandscapeLayerMap>(node);
}

void TerrainSurfaceToolEditorPlugin::setupMenu()
{
    pluginMenu = WindowManager::findMenu("Sogeclair");
    if (!pluginMenu)
    {
        if (auto* mainWindow = qobject_cast<QMainWindow*>(QApplication::activeWindow()))
        {
            pluginMenu = new QMenu("Sogeclair", mainWindow);
            if (mainWindow->menuBar())
                mainWindow->menuBar()->addMenu(pluginMenu);
            ownsMenu = true;
        }
        else
        {
            pluginMenu = new QMenu("Sogeclair");
            ownsMenu = true;
        }
    }

    if (!pluginMenu)
    {
        Log::error("[TerrainSurfaceTool] Failed to create menu action: pluginMenu is null\n");
        return;
    }

    terrainToolAction = new QAction("Terrain Surface Tool", pluginMenu);
    connect(terrainToolAction, &QAction::triggered,
            this, &TerrainSurfaceToolEditorPlugin::openTerrainTool);

    pluginMenu->addAction(terrainToolAction);
}

void TerrainSurfaceToolEditorPlugin::openTerrainTool()
{
    if (!terrainToolPanel)
        return;

    WindowManager::show(terrainToolPanel.get());
    WindowManager::activate(terrainToolPanel.get());
}
}
