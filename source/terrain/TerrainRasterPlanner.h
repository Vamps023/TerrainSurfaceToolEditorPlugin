#pragma once

#include "../rasterizer/SurfaceRasterizer.h"

#include <UnigineObjects.h>

#include <vector>

class TerrainRasterPlanner
{
public:
    struct TileRasterPlan
    {
        Unigine::LandscapeLayerMapPtr tile;
        SurfaceRasterizer::RasterBuffer rasterBuffer;
        int mergedSurfaceCount = 0;
    };

    [[nodiscard]] static std::vector<TileRasterPlan> buildHeightPlans(
        const std::vector<Unigine::NodePtr>& nodes,
        const std::vector<Unigine::LandscapeLayerMapPtr>& tiles,
        const SurfaceRasterizer::SurfaceQuery& query);

    [[nodiscard]] static std::vector<TileRasterPlan> buildMaskPlans(
        const std::vector<Unigine::NodePtr>& nodes,
        const std::vector<Unigine::LandscapeLayerMapPtr>& tiles,
        const SurfaceRasterizer::SurfaceQuery& query);

private:
    enum class RasterMode
    {
        Height,
        Mask,
    };

    static std::vector<TileRasterPlan> buildPlans(
        const std::vector<Unigine::NodePtr>& nodes,
        const std::vector<Unigine::LandscapeLayerMapPtr>& tiles,
        const SurfaceRasterizer::SurfaceQuery& query,
        RasterMode mode);

    static bool rasterizeSurface(const Unigine::LandscapeLayerMapPtr& tile,
                                 const ObjectSurface& objectSurface,
                                 RasterMode mode,
                                 SurfaceRasterizer::RasterBuffer& outBuffer);
};
