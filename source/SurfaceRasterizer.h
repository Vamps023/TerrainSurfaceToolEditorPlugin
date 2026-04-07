#pragma once

#include <UnigineImage.h>
#include <UnigineMathLib.h>
#include <UnigineMeshStatic.h>
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
        bool use_regex = false;
        std::regex regex;
    };

    struct RasterBuffer
    {
        Unigine::Math::ivec2 resolution = Unigine::Math::ivec2_zero;
        std::vector<float> values;
        std::vector<float> alpha;
        std::vector<int> source_index;
        std::vector<int> seeds;

        void reset(const Unigine::Math::ivec2& resolution_value);
        bool empty() const { return seeds.empty(); }
    };

    static bool buildSurfaceQuery(const std::string& pattern, SurfaceQuery& out_query, std::string& error_message);
    static std::vector<int> findMatchingSurfaceIds(const Unigine::ObjectMeshStaticPtr& mesh, const SurfaceQuery& query);
    static std::vector<Unigine::NodePtr> collectMeshNodesRecursive(const std::vector<Unigine::NodePtr>& roots);

    static bool extractSurfaceVerticesWorldSpace(const Unigine::ObjectMeshStaticPtr& mesh,
                                                 int surface_id,
                                                 std::vector<Unigine::Math::vec3>& out_vertices);
    static void buildMidpointPath(const std::vector<Unigine::Math::vec3>& surface_vertices,
                                  std::vector<Unigine::Math::vec3>& out_midpoints);
    static void samplePolyline(const std::vector<Unigine::Math::vec3>& points,
                               double spacing,
                               std::vector<Unigine::Math::vec3>& out_samples);

    static bool rasterizeSurfaceHeight(const Unigine::LandscapeLayerMapPtr& terrain_tile,
                                       const ObjectSurface& object_surface,
                                       RasterBuffer& out_buffer);
    static bool rasterizeSurfaceMask(const Unigine::LandscapeLayerMapPtr& terrain_tile,
                                     const ObjectSurface& object_surface,
                                     RasterBuffer& out_buffer);

    static void applyDistanceFalloff(const Unigine::LandscapeLayerMapPtr& terrain_tile,
                                     RasterBuffer& buffer,
                                     double flat_distance,
                                     double falloff_distance);

    static Unigine::ImagePtr createHeightImage(const RasterBuffer& buffer);
    static Unigine::ImagePtr createHeightAlphaImage(const Unigine::ImagePtr& height_image);
    static Unigine::ImagePtr createMaskImage(const RasterBuffer& buffer);

private:
    static constexpr float kEpsilon = 1e-6f;

    static int toIndex(int x, int y, int width);
    static int toImageY(int y, int height);
    static bool pointInTriangle(const Unigine::Math::Vec3& point,
                                const Unigine::Math::Vec3& v1,
                                const Unigine::Math::Vec3& v2,
                                const Unigine::Math::Vec3& v3,
                                double& out_u,
                                double& out_v);
    static bool appendSurfaceTrianglesWorldSpace(const ObjectSurface& object_surface,
                                                 Unigine::Vector<Unigine::Math::dvec3>& out_vertices);
    static void collectMeshNodesRecursive(const Unigine::NodePtr& node,
                                          std::vector<Unigine::NodePtr>& out_nodes,
                                          std::unordered_set<int>& visited_node_ids,
                                          std::unordered_set<int>& collected_mesh_ids);
};
