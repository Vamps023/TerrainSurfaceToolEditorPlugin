#pragma once

#include <UnigineImage.h>
#include <UnigineMathLib.h>
#include <UnigineMesh.h>
#include <UnigineNode.h>
#include <UnigineNodes.h>
#include <UnigineObjects.h>

#include "SurfaceQuery.h"

#include <string>
#include <unordered_set>
#include <vector>

// Pair of (Object, surface index) that identifies a single mesh surface.
using ObjectSurface = std::pair<Unigine::ObjectPtr, int>;

// Utility class that converts mesh surfaces into per-tile raster buffers
// (heightmap or mask) and applies distance-based falloff blending.
class SurfaceRasterizer
{
public:
    // Re-export the global SurfaceQuery as a nested type so existing
    // SurfaceRasterizer::SurfaceQuery references continue to compile.
    using SurfaceQuery = ::SurfaceQuery;

    // Per-tile CPU raster buffer storing world-space heights and blend alphas
    // for every pixel of a LandscapeLayerMap tile.
    struct RasterBuffer
    {
        Unigine::Math::ivec2 resolution = Unigine::Math::ivec2_zero;
        std::vector<float> values;      // world-space height per pixel
        std::vector<float> alpha;       // blend opacity [0..1] per pixel
        std::vector<int> sourceIndex;   // nearest rasterised seed index (-1 = untouched)
        std::vector<int> seeds;         // pixel indices directly covered by mesh triangles

        // Resets all arrays to match the given tile resolution.
        void reset(const Unigine::Math::ivec2& resolutionValue);
        // Returns true when no mesh triangles have been rasterised yet.
        bool empty() const { return seeds.empty(); }
    };

    // Axis-aligned sub-rectangle of a tile that was actually modified,
    // used to issue minimal asyncTextureDraw calls.
    struct RasterRegion
    {
        Unigine::Math::ivec2 coord = Unigine::Math::ivec2_zero; // top-left pixel
        Unigine::Math::ivec2 size = Unigine::Math::ivec2_zero;  // width/height in pixels

        bool valid() const { return size.x > 0 && size.y > 0; }
    };

    // Compiles a raw pattern string into a SurfaceQuery (exact or regex).
    // Returns false and fills errorMessage on failure.
    static bool buildSurfaceQuery(const std::string& pattern, SurfaceQuery& outQuery, std::string& errorMessage);

    // Returns the indices of all surfaces on mesh whose name matches query.
    static std::vector<int> findMatchingSurfaceIds(const Unigine::ObjectMeshStaticPtr& mesh, const SurfaceQuery& query);

    // Recursively collects all ObjectMeshStatic nodes under roots.
    static std::vector<Unigine::NodePtr> collectMeshNodesRecursive(const std::vector<Unigine::NodePtr>& roots);

    // Rasterizes the triangles of objectSurface into outBuffer as world-space heights.
    // Only up-facing triangles (normal.z >= kMinTerrainSurfaceNormalZ) are written.
    // Returns true if at least one pixel was written.
    static bool rasterizeSurfaceHeight(const Unigine::LandscapeLayerMapPtr& terrainTile,
                                       const ObjectSurface& objectSurface,
                                       RasterBuffer& outBuffer);

    // Rasterizes the footprint of objectSurface as a binary mask (value = 1 for covered pixels).
    // Returns true if at least one pixel was written.
    static bool rasterizeSurfaceMask(const Unigine::LandscapeLayerMapPtr& terrainTile,
                                     const ObjectSurface& objectSurface,
                                     RasterBuffer& outBuffer);

    // Merges source pixels (alpha > 0) into target, overwriting existing data.
    // Returns true if any pixel was merged.
    static bool mergeRasterBuffer(RasterBuffer& target, const RasterBuffer& source);

    // Expands the rasterized footprint outward using a Chamfer 3-4 distance transform.
    // Pixels within flatDistance keep alpha=1; pixels in the falloff band get a
    // smoothstep-blended alpha that fades to 0 at flatDistance + falloffDistance.
    static void applyDistanceFalloff(const Unigine::LandscapeLayerMapPtr& terrainTile,
                                     RasterBuffer& buffer,
                                     double flatDistance,
                                     double falloffDistance);

    // Blends the falloff-region heights in `buffer` with the existing terrain height
    // sampled via `fetch`. After this call, pixels inside the touched region hold the
    // final blended world-space height and their alpha is set to 1.0 (fully opaque),
    // so the GPU brush can simply overwrite height + opacity without further blending.
    // If clamp_to_original is true, blended height will never go below original terrain height.
    static void blendFalloffWithExistingTerrain(const Unigine::LandscapeLayerMapPtr& terrainTile,
                                                const Unigine::LandscapeFetchPtr& fetch,
                                                RasterBuffer& buffer,
                                                bool clampToOriginal = false);

    // Fills alpha=0 pixels with the existing terrain height so the brush never
    // writes height=0 outside the painted region (prevents black ring/pit artifacts).
    static void fillUnpaintedPixelsWithTerrain(const Unigine::LandscapeLayerMapPtr& terrainTile,
                                               const Unigine::LandscapeFetchPtr& fetch,
                                               RasterBuffer& buffer);

    // Creates a full-tile RGBA32F image from buffer (R=height, A=alpha).
    static Unigine::ImagePtr createHeightImage(const RasterBuffer& buffer);
    // Creates a full-tile RGBA32F image with all channels = source alpha (used as opacity mask).
    static Unigine::ImagePtr createHeightAlphaImage(const Unigine::ImagePtr& heightImage);
    // Creates a full-tile R8 image from buffer alpha values (used for landscape mask).
    static Unigine::ImagePtr createMaskImage(const RasterBuffer& buffer);
    // Returns the bounding rectangle of all pixels with alpha > 0.
    static RasterRegion calculateTouchedRegion(const RasterBuffer& buffer);
    // Region-cropped variants of the above — only generate an image for the touched sub-rect.
    static Unigine::ImagePtr createHeightImage(const RasterBuffer& buffer, const RasterRegion& region);
    static Unigine::ImagePtr createHeightAlphaImage(const Unigine::ImagePtr& heightImage, const RasterRegion& region);
    static Unigine::ImagePtr createMaskImage(const RasterBuffer& buffer, const RasterRegion& region);

private:
    static constexpr float kEpsilon = 1e-6f;

    static int toIndex(int x, int y, int width);
    static bool pointInTriangle(const Unigine::Math::Vec3& point,
                                const Unigine::Math::Vec3& v1,
                                const Unigine::Math::Vec3& v2,
                                const Unigine::Math::Vec3& v3,
                                double& outU,
                                double& outV);
    static bool isUpFacingTriangle(const Unigine::Math::dvec3& v1,
                                   const Unigine::Math::dvec3& v2,
                                   const Unigine::Math::dvec3& v3);
    static bool appendSurfaceTrianglesWorldSpace(const ObjectSurface& objectSurface,
                                                 Unigine::Vector<Unigine::Math::dvec3>& outVertices);
};
