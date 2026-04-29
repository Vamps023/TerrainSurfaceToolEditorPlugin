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
    Unigine::ObjectLandscapeTerrainPtr getLandscapeTerrainById(int nodeId) const;
    std::vector<Unigine::LandscapeLayerMapPtr> getLandscapeLayerMaps(
        const Unigine::ObjectLandscapeTerrainPtr& terrain) const;
    Unigine::LandscapeLayerMapPtr getLandscapeLayerMapById(int nodeId) const;
    TerrainManipulator* manipulator() const { return terrainManipulator.get(); }

private slots:
    void openTerrainTool();

private:
    void setupMenu();

    std::unique_ptr<LandscapeSaveManager> saveManager;
    std::unique_ptr<TerrainManipulator> terrainManipulator;
    std::unique_ptr<TerrainToolPanel> terrainToolPanel;

    QMenu* pluginMenu = nullptr;
    QAction* terrainToolAction = nullptr;
    bool ownsMenu = false;
};
}
