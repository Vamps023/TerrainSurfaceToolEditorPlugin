#include "TerrainToolController.h"

#include "../core/TerrainSurfaceToolEditorPlugin.h"
#include "../terrain/TerrainManipulator.h"

#include <QSet>

#include <UnigineObjects.h>

#include <algorithm>

using namespace Unigine;

TerrainToolController::TerrainToolController(UnigineEditor::TerrainSurfaceToolEditorPlugin* plugin)
    : plugin(plugin)
{
}

QStringList TerrainToolController::selectedSurfaceNames() const
{
    QStringList result;
    if (!plugin)
        return result;

    QSet<QString> names;
    const std::vector<NodePtr> selectedNodes = plugin->getSelectedMeshNodes();
    for (const auto& node : selectedNodes)
    {
        if (!node || node->getType() != Node::OBJECT_MESH_STATIC)
            continue;

        ObjectMeshStaticPtr mesh = checked_ptr_cast<ObjectMeshStatic>(node);
        if (!mesh)
            continue;

        for (int surfaceIndex = 0; surfaceIndex < mesh->getNumSurfaces(); ++surfaceIndex)
        {
            const QString name = QString::fromUtf8(mesh->getSurfaceName(surfaceIndex));
            if (!name.isEmpty())
                names.insert(name);
        }
    }

    result = names.values();
    std::sort(result.begin(), result.end());
    return result;
}

bool TerrainToolController::hasSelectionChanged()
{
    // Fast path: walk all nodes only when we actually need to rebuild the name set.
    const QStringList currentNames = selectedSurfaceNames();
    QSet<QString> currentIds;
    for (const QString& name : currentNames)
        currentIds.insert(name);

    if (currentIds == previousSelectionIds)
        return false;

    previousSelectionIds = std::move(currentIds);
    return true;
}

QStringList TerrainToolController::maskSlotLabels() const
{
    static constexpr const char* kChannels = "RGBA";
    QStringList labels;

    // Try to get the active terrain so we can read user-defined mask names.
    const auto terrains = plugin ? plugin->getLandscapeTerrains() : std::vector<ObjectLandscapeTerrainPtr>();
    const ObjectLandscapeTerrainPtr terrain = !terrains.empty()
        ? terrains.front()
        : Landscape::getActiveTerrain();

    for (int maskIndex = 0; maskIndex <= kMaxLandscapeMaskIndex; ++maskIndex)
    {
        const int maskPage = maskIndex / 4;
        const QChar channel(kChannels[maskIndex % 4]);
        const QString techSuffix = QString("(mask_%1 / %2)").arg(maskPage).arg(channel);

        QString displayName;
        if (terrain)
        {
            const auto detailMask = terrain->getDetailMask(maskIndex);
            if (detailMask)
            {
                const char* name = detailMask->getName();
                if (name && name[0] != '\0')
                    displayName = QString::fromUtf8(name);
            }
        }

        if (displayName.isEmpty())
            displayName = QString("Mask %1").arg(maskIndex);

        labels.append(QString("%1  %2").arg(displayName, techSuffix));
    }

    return labels;
}

QVector<TerrainToolController::TileOption> TerrainToolController::landscapeTileOptions() const
{
    QVector<TileOption> options;
    options.push_back(TileOption{"All Tiles", -1});
    if (!plugin)
        return options;

    const auto terrains = plugin->getLandscapeTerrains();
    const ObjectLandscapeTerrainPtr terrain = !terrains.empty() ? terrains.front() : Landscape::getActiveTerrain();
    if (!terrain)
        return options;

    const auto layerMaps = plugin->getLandscapeLayerMaps(terrain);
    for (const auto& layerMap : layerMaps)
    {
        if (!layerMap)
            continue;

        QString tileName = QString::fromUtf8(layerMap->getName());
        if (tileName.trimmed().isEmpty())
            tileName = QString("LandscapeLayerMap %1").arg(layerMap->getID());
        options.push_back(TileOption{tileName, layerMap->getID()});
    }

    return options;
}

TerrainToolController::ApplyResult TerrainToolController::pullTerrainToSurface(
    int targetTileId,
    const std::string& surfaceName,
    const TerrainBrushSettings& settings,
    const LogFn& log) const
{
    const std::vector<NodePtr> selectedNodes = plugin ? plugin->getSelectedMeshNodes() : std::vector<NodePtr>();
    ApplyResult result = validateSelection(selectedNodes, true, surfaceName);
    if (!result.error.isEmpty())
        return result;

    const TargetContext target = resolveTarget(targetTileId);
    if (!target.error.isEmpty())
    {
        result.error = target.error;
        return result;
    }

    result.queued = plugin->manipulator()->pullTerrainToSurface(
        selectedNodes, target.terrain, target.tile, surfaceName, settings, log);
    return result;
}

TerrainToolController::ApplyResult TerrainToolController::applyLandscapeMask(
    int targetTileId,
    const std::string& surfaceName,
    const TerrainBrushSettings& settings,
    int maskIndex,
    const LogFn& log) const
{
    const std::vector<NodePtr> selectedNodes = plugin ? plugin->getSelectedMeshNodes() : std::vector<NodePtr>();
    ApplyResult result = validateSelection(selectedNodes, true, surfaceName);
    if (!result.error.isEmpty())
        return result;

    const TargetContext target = resolveTarget(targetTileId);
    if (!target.error.isEmpty())
    {
        result.error = target.error;
        return result;
    }

    result.queued = plugin->manipulator()->applyLandscapeMask(
        selectedNodes, target.terrain, target.tile, surfaceName, settings, maskIndex, log);
    return result;
}

TerrainToolController::ApplyResult TerrainToolController::eraseHeight(
    int targetTileId,
    const TerrainBrushSettings& settings,
    const LogFn& log) const
{
    ApplyResult result;
    if (!plugin || !plugin->manipulator())
    {
        result.error = "ERROR: Plugin is not initialized.";
        return result;
    }

    const TargetContext target = resolveTarget(targetTileId);
    if (!target.error.isEmpty())
    {
        result.error = target.error;
        return result;
    }

    // Erase Height does not require mesh selection — it erases terrain heightmap directly.
    result.queued = plugin->manipulator()->eraseHeight(
        std::vector<NodePtr>(), target.terrain, target.tile, settings, log);
    return result;
}

TerrainBrushSettings TerrainToolController::currentSettings(double brushSize,
                                                            double flatDistance,
                                                            double falloffDistance)
{
    TerrainBrushSettings settings;
    settings.brushSize = brushSize;
    settings.flatDistance = flatDistance;
    settings.falloffDistance = falloffDistance;
    settings.smoothingStrength = 0.5; // reserved for future smoothing pass
    settings.clampToOriginal = false;
    return settings;
}

TerrainToolController::TargetContext TerrainToolController::resolveTarget(int targetTileId) const
{
    TargetContext context;
    if (!plugin || !plugin->manipulator())
    {
        context.error = "ERROR: Plugin is not initialized.";
        return context;
    }

    const auto terrains = plugin->getLandscapeTerrains();
    context.terrain = !terrains.empty() ? terrains.front() : Landscape::getActiveTerrain();
    context.tile = targetTileId >= 0 ? plugin->getLandscapeLayerMapById(targetTileId) : nullptr;
    if (!context.terrain)
        context.error = "ERROR: No landscape selected.";
    return context;
}

TerrainToolController::ApplyResult TerrainToolController::validateSelection(
    const std::vector<NodePtr>& selectedNodes,
    bool requireSurfaceName,
    const std::string& surfaceName) const
{
    ApplyResult result;
    result.selectedMeshCount = static_cast<int>(selectedNodes.size());
    if (!plugin || !plugin->manipulator())
    {
        result.error = "ERROR: Plugin is not initialized.";
        return result;
    }
    if (selectedNodes.empty())
    {
        result.error = "ERROR: No mesh nodes selected.";
        return result;
    }
    if (requireSurfaceName && surfaceName.empty())
        result.error = "ERROR: Surface name is empty.";
    return result;
}
