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

// Controller that mediates between the TerrainToolPanel (UI) and the
// TerrainManipulator (business logic). All non-trivial operations and
// data transformations live here so the panel remains a thin view layer.
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

    // ---- Data queries for populating UI combo boxes ----

    // Returns sorted unique surface names from the currently selected mesh nodes.
    QStringList selectedSurfaceNames() const;

    // Returns landscape tile options for the dropdown ("All Tiles" + per-tile entries).
    QVector<TileOption> landscapeTileOptions() const;

    // Returns human-readable labels for each landscape mask slot [0..19].
    QStringList maskSlotLabels() const;

    // Returns true if the set of selected mesh surface names changed since the last call.
    // Internally tracks the previous selection so the panel does not need to.
    bool hasSelectionChanged();

    // ---- Action methods ----

    ApplyResult pullTerrainToSurface(int targetTileId,
                                     const std::string& surfaceName,
                                     const TerrainBrushSettings& settings,
                                     const LogFn& log) const;
    ApplyResult applyLandscapeMask(int targetTileId,
                                   const std::string& surfaceName,
                                   const TerrainBrushSettings& settings,
                                   int maskIndex,
                                   const LogFn& log) const;
    ApplyResult eraseHeight(int targetTileId,
                            const TerrainBrushSettings& settings,
                            const LogFn& log) const;

    // ---- Static helpers ----

    // Builds a TerrainBrushSettings from raw UI values.
    static TerrainBrushSettings currentSettings(double brushSize,
                                                double flatDistance,
                                                double falloffDistance);

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
    QSet<QString> previousSelectionIds;
};
