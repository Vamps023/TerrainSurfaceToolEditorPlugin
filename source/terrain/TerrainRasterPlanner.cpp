#include "TerrainRasterPlanner.h"

using namespace Unigine;

std::vector<TerrainRasterPlanner::TileRasterPlan> TerrainRasterPlanner::buildHeightPlans(
    const std::vector<NodePtr>& nodes,
    const std::vector<LandscapeLayerMapPtr>& tiles,
    const SurfaceRasterizer::SurfaceQuery& query)
{
    return buildPlans(nodes, tiles, query, RasterMode::Height);
}

std::vector<TerrainRasterPlanner::TileRasterPlan> TerrainRasterPlanner::buildMaskPlans(
    const std::vector<NodePtr>& nodes,
    const std::vector<LandscapeLayerMapPtr>& tiles,
    const SurfaceRasterizer::SurfaceQuery& query)
{
    return buildPlans(nodes, tiles, query, RasterMode::Mask);
}

std::vector<TerrainRasterPlanner::TileRasterPlan> TerrainRasterPlanner::buildPlans(
    const std::vector<NodePtr>& nodes,
    const std::vector<LandscapeLayerMapPtr>& tiles,
    const SurfaceRasterizer::SurfaceQuery& query,
    RasterMode mode)
{
    std::vector<TileRasterPlan> plans;

    for (const auto& tile : tiles)
    {
        if (!tile)
            continue;

        TileRasterPlan plan;
        plan.tile = tile;
        plan.rasterBuffer.reset(tile->getResolution());

        for (const auto& node : nodes)
        {
            if (!node || node->getType() != Node::OBJECT_MESH_STATIC)
                continue;

            ObjectMeshStaticPtr mesh = checked_ptr_cast<ObjectMeshStatic>(node);
            if (!mesh || !tile->getWorldBoundBox().insideValid(mesh->getWorldBoundBox()))
                continue;

            const std::vector<int> surfaceIds = SurfaceRasterizer::findMatchingSurfaceIds(mesh, query);
            for (int surfaceId : surfaceIds)
            {
                ObjectSurface objectSurface = std::make_pair(static_ptr_cast<Object>(mesh), surfaceId);
                SurfaceRasterizer::RasterBuffer surfaceRasterBuffer;
                if (!rasterizeSurface(tile, objectSurface, mode, surfaceRasterBuffer))
                    continue;

                if (SurfaceRasterizer::mergeRasterBuffer(plan.rasterBuffer, surfaceRasterBuffer))
                    ++plan.mergedSurfaceCount;
            }
        }

        if (!plan.rasterBuffer.empty())
            plans.push_back(plan);
    }

    return plans;
}

bool TerrainRasterPlanner::rasterizeSurface(const LandscapeLayerMapPtr& tile,
                                            const ObjectSurface& objectSurface,
                                            RasterMode mode,
                                            SurfaceRasterizer::RasterBuffer& outBuffer)
{
    if (mode == RasterMode::Mask)
        return SurfaceRasterizer::rasterizeSurfaceMask(tile, objectSurface, outBuffer);

    return SurfaceRasterizer::rasterizeSurfaceHeight(tile, objectSurface, outBuffer);
}
