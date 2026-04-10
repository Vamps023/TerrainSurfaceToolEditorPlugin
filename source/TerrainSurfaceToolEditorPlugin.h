#pragma once

#include <editor/UniginePlugin.h>

#include <QObject>
#include <QAction>
#include <QMenu>

#include <UnigineNode.h>
#include <UnigineObjects.h>

#include <memory>
#include <unordered_set>
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
    std::vector<Unigine::ObjectLandscapeTerrainPtr> getLandscapeTerrains() const;
    Unigine::ObjectLandscapeTerrainPtr getLandscapeTerrainById(int node_id) const;
    std::vector<Unigine::LandscapeLayerMapPtr> getLandscapeLayerMaps(
        const Unigine::ObjectLandscapeTerrainPtr& terrain) const;
    Unigine::LandscapeLayerMapPtr getLandscapeLayerMapById(int node_id) const;
    TerrainManipulator* manipulator() const { return terrain_manipulator_.get(); }

private slots:
    void openTerrainTool();

private:
    void setupMenu();
    static void collectLandscapeTerrainsRecursive(const Unigine::NodePtr& node,
                                                  std::vector<Unigine::ObjectLandscapeTerrainPtr>& out_terrains,
                                                  std::unordered_set<int>& visited_node_ids);
    static void collectLandscapeLayerMapsRecursive(const Unigine::NodePtr& node,
                                                   std::vector<Unigine::LandscapeLayerMapPtr>& out_layer_maps,
                                                   std::unordered_set<int>& visited_node_ids);

    std::unique_ptr<LandscapeSaveManager> save_manager_;
    std::unique_ptr<TerrainManipulator> terrain_manipulator_;
    std::unique_ptr<TerrainToolPanel> terrain_tool_panel_;

    QMenu* vamps_menu_ = nullptr;
    QAction* terrain_tool_action_ = nullptr;
    bool owns_menu_ = false;
};
}
