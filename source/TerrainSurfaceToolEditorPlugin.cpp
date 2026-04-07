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

void TerrainSurfaceToolEditorPlugin::openTerrainTool()
{
    if (!terrain_tool_panel_)
        return;

    WindowManager::show(terrain_tool_panel_.get());
    WindowManager::activate(terrain_tool_panel_.get());
}
}
