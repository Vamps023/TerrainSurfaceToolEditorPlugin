#pragma once

#include <UnigineImage.h>
#include <UnigineMathLib.h>
#include <UnigineMesh.h>
#include <UnigineNode.h>
#include <UnigineNodes.h>
#include <UnigineObjects.h>

#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

using ObjectSurface = std::pair<Unigine::ObjectPtr, int>;

class SurfaceRasterizer
{
public:
    struct SurfaceQuery
    {
        std::string raw;
        bool useRegex = false;
        std::regex regex;
    };

    struct RasterBuffer
    {
        Unigine::Math::ivec2 resolution = Unigine::Math::ivec2_zero;
        std::vector<float> values;
        std::vector<float> alpha;
        std::vector<int> sourceIndex;
        std::vector<int> seeds;

        void reset(const Unigine::Math::ivec2& resolutionValue);
        bool empty() const { return seeds.empty(); }
    };

    struct RasterRegion
    {
        Unigine::Math::ivec2 coord = Unigine::Math::ivec2_zero;
        Unigine::Math::ivec2 size = Unigine::Math::ivec2_zero;

        bool valid() const { return size.x > 0 && size.y > 0; }
    };

    static bool buildSurfaceQuery(const std::string& pattern, SurfaceQuery& outQuery, std::string& errorMessage);
    static std::vector<int> findMatchingSurfaceIds(const Unigine::ObjectMeshStaticPtr& mesh, const SurfaceQuery& query);
    static std::vector<Unigine::NodePtr> collectMeshNodesRecursive(const std::vector<Unigine::NodePtr>& roots);

    static bool extractSurfaceVerticesWorldSpace(const Unigine::ObjectMeshStaticPtr& mesh,
                                                 int surfaceId,
                                                 std::vector<Unigine::Math::vec3>& outVertices);
    static void buildMidpointPath(const std::vector<Unigine::Math::vec3>& surfaceVertices,
                                  std::vector<Unigine::Math::vec3>& outMidpoints);
    static void samplePolyline(const std::vector<Unigine::Math::vec3>& points,
                               double spacing,
                               std::vector<Unigine::Math::vec3>& outSamples);

    static bool rasterizeSurfaceHeight(const Unigine::LandscapeLayerMapPtr& terrainTile,
                                       const ObjectSurface& objectSurface,
                                       RasterBuffer& outBuffer);
    static bool rasterizeSurfaceMask(const Unigine::LandscapeLayerMapPtr& terrainTile,
                                     const ObjectSurface& objectSurface,
                                     RasterBuffer& outBuffer);
    static bool mergeRasterBuffer(RasterBuffer& target, const RasterBuffer& source);

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

    static Unigine::ImagePtr createHeightImage(const RasterBuffer& buffer);
    static Unigine::ImagePtr createHeightAlphaImage(const Unigine::ImagePtr& heightImage);
    static Unigine::ImagePtr createMaskImage(const RasterBuffer& buffer);
    static RasterRegion calculateTouchedRegion(const RasterBuffer& buffer);
    static Unigine::ImagePtr createHeightImage(const RasterBuffer& buffer, const RasterRegion& region);
    static Unigine::ImagePtr createHeightAlphaImage(const Unigine::ImagePtr& heightImage, const RasterRegion& region);
    static Unigine::ImagePtr createMaskImage(const RasterBuffer& buffer, const RasterRegion& region);

private:
    static constexpr float kEpsilon = 1e-6f;

    static int toIndex(int x, int y, int width);
    static int toImageY(int y, int height);
    static bool pointInTriangle(const Unigine::Math::Vec3& point,
                                const Unigine::Math::Vec3& v1,
                                const Unigine::Math::Vec3& v2,
                                const Unigine::Math::Vec3& v3,
                                double& outU,
                                double& outV);
    static bool appendSurfaceTrianglesWorldSpace(const ObjectSurface& objectSurface,
                                                 Unigine::Vector<Unigine::Math::dvec3>& outVertices);
};
