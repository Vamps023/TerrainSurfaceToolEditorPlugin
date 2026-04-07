#pragma once

#include <editor/UniginePlugin.h>

#include <QObject>
#include <QAction>
#include <QMenu>

#include <UnigineNode.h>

#include <memory>
#include <vector>

class LandscapeSaveManager;
class TerrainManipulator;
class TerrainToolPanel;

namespace UnigineEditor
{
class TerrainSurfaceToolEditorPlugin : public QObject, public Plugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID UNIGINE_EDITOR_PLUGIN_IID FILE "TerrainSurfaceToolEditorPlugin.json")
    Q_INTERFACES(UnigineEditor::Plugin)

public:
    TerrainSurfaceToolEditorPlugin();
    ~TerrainSurfaceToolEditorPlugin() override;

    bool init() override;
    void shutdown() override;

    std::vector<Unigine::NodePtr> getSelectedMeshNodes() const;
    TerrainManipulator* manipulator() const { return terrain_manipulator_.get(); }

private slots:
    void openTerrainTool();

private:
    void setupMenu();

    std::unique_ptr<LandscapeSaveManager> save_manager_;
    std::unique_ptr<TerrainManipulator> terrain_manipulator_;
    std::unique_ptr<TerrainToolPanel> terrain_tool_panel_;

    QMenu* vamps_menu_ = nullptr;
    QAction* terrain_tool_action_ = nullptr;
    bool owns_menu_ = false;
};
}
