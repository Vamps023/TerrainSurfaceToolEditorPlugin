#pragma once

#include "../terrain/TerrainBrushSettings.h"

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <UnigineObjects.h>

#include <functional>
#include <string>
#include <vector>

namespace UnigineEditor
{
class TerrainSurfaceToolEditorPlugin;
}

class TerrainToolController
{
public:
    using LogFn = std::function<void(const std::string&)>;

    struct TileOption
    {
        QString label;
        int nodeId = -1;
    };

    struct ApplyResult
    {
        bool queued = false;
        int selectedMeshCount = 0;
        QString error;
    };

    explicit TerrainToolController(UnigineEditor::TerrainSurfaceToolEditorPlugin* plugin);

    QStringList selectedSurfaceNames() const;
    QVector<TileOption> landscapeTileOptions() const;

    // Returns true if the set of selected mesh surface names changed since the last call.
    // Internally tracks the previous selection so the panel does not need to.
    bool hasSelectionChanged();

    ApplyResult pullTerrainToSurface(int targetTileId,
                                     const std::string& surfaceName,
                                     const TerrainBrushSettings& settings,
                                     const LogFn& log) const;
    ApplyResult applyLandscapeMask(int targetTileId,
                                   const std::string& surfaceName,
                                   const TerrainBrushSettings& settings,
                                   int maskIndex,
                                   const LogFn& log) const;
    ApplyResult paintWhiteHeight(int targetTileId,
                                 const TerrainBrushSettings& settings,
                                 const LogFn& log) const;

private:
    struct TargetContext
    {
        Unigine::ObjectLandscapeTerrainPtr terrain;
        Unigine::LandscapeLayerMapPtr tile;
        QString error;
    };

    TargetContext resolveTarget(int targetTileId) const;
    ApplyResult validateSelection(const std::vector<Unigine::NodePtr>& selectedNodes,
                                  bool requireSurfaceName,
                                  const std::string& surfaceName) const;

    UnigineEditor::TerrainSurfaceToolEditorPlugin* plugin = nullptr;
    QSet<int> previousSelectionIds;
};
