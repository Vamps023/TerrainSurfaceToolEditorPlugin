#include "SurfaceRasterizer.h"

#include "../core/NodeTreeWalker.h"

#include <UnigineLog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

using namespace Unigine;
using namespace Unigine::Math;

void SurfaceRasterizer::RasterBuffer::reset(const ivec2& resolution_value)
{
    resolution = resolution_value;
    const int pixel_count = resolution.x * resolution.y;
    values.assign(pixel_count, 0.0f);
    alpha.assign(pixel_count, 0.0f);
    source_index.assign(pixel_count, -1);
    seeds.clear();
}

bool SurfaceRasterizer::buildSurfaceQuery(const std::string& pattern, SurfaceQuery& out_query, std::string& error_message)
{
    out_query.raw = pattern;
    out_query.use_regex = false;

    if (pattern.empty())
    {
        error_message = "Surface name is empty.";
        return false;
    }

    try
    {
        out_query.regex = std::regex(pattern);
        out_query.use_regex = true;
    }
    catch (const std::regex_error&)
    {
        out_query.use_regex = false;
    }

    return true;
}

std::vector<int> SurfaceRasterizer::findMatchingSurfaceIds(const ObjectMeshStaticPtr& mesh, const SurfaceQuery& query)
{
    std::vector<int> surface_ids;
    if (!mesh)
        return surface_ids;

    const int exact_surface_id = mesh->findSurface(query.raw.c_str());
    if (exact_surface_id >= 0)
    {
        surface_ids.push_back(exact_surface_id);
        return surface_ids;
    }

    if (!query.use_regex)
        return surface_ids;

    const int num_surfaces = mesh->getNumSurfaces();
    surface_ids.reserve(num_surfaces);
    for (int surface_index = 0; surface_index < num_surfaces; ++surface_index)
    {
        const std::string surface_name = mesh->getSurfaceName(surface_index);
        if (std::regex_search(surface_name, query.regex))
            surface_ids.push_back(surface_index);
    }

    return surface_ids;
}

std::vector<NodePtr> SurfaceRasterizer::collectMeshNodesRecursive(const std::vector<NodePtr>& roots)
{
    std::vector<NodePtr> collected_nodes;
    std::unordered_set<int> visited_node_ids;
    std::unordered_set<int> collected_mesh_ids;

    for (const auto& root : roots)
        NodeTreeWalker::collectMeshNodesRecursive(root, collected_nodes, visited_node_ids, collected_mesh_ids);

    return collected_nodes;
}

bool SurfaceRasterizer::extractSurfaceVerticesWorldSpace(const ObjectMeshStaticPtr& mesh,
                                                         int surface_id,
                                                         std::vector<vec3>& out_vertices)
{
    out_vertices.clear();
    if (!mesh || surface_id < 0)
        return false;

    auto mesh_static = mesh->getMeshForce();
    if (!mesh_static)
        return false;

    const dmat4 world_transform = mesh->getWorldTransform();
    const int vertex_count = mesh_static->getNumVertices(surface_id);
    out_vertices.reserve(vertex_count);

    for (int vertex_index = 0; vertex_index < vertex_count; ++vertex_index)
    {
        const vec3 mesh_vertex = mesh_static->getVertex(vertex_index, surface_id);
        const dvec3 world_vertex = world_transform * dvec3(mesh_vertex.x, mesh_vertex.y, mesh_vertex.z);
        out_vertices.emplace_back(static_cast<float>(world_vertex.x),
                                  static_cast<float>(world_vertex.y),
                                  static_cast<float>(world_vertex.z));
    }

    return !out_vertices.empty();
}

void SurfaceRasterizer::buildMidpointPath(const std::vector<vec3>& surface_vertices,
                                          std::vector<vec3>& out_midpoints)
{
    out_midpoints.clear();
    if (surface_vertices.size() < 2)
        return;

    out_midpoints.reserve(surface_vertices.size() / 2);
    for (size_t index = 0; index + 1 < surface_vertices.size(); index += 2)
    {
        const vec3& p1 = surface_vertices[index];
        const vec3& p2 = surface_vertices[index + 1];
        out_midpoints.emplace_back((p1.x + p2.x) * 0.5f,
                                   (p1.y + p2.y) * 0.5f,
                                   (p1.z + p2.z) * 0.5f);
    }
}

void SurfaceRasterizer::samplePolyline(const std::vector<vec3>& points,
                                       double spacing,
                                       std::vector<vec3>& out_samples)
{
    out_samples.clear();
    if (points.empty())
        return;

    if (points.size() == 1 || spacing <= kEpsilon)
    {
        out_samples = points;
        return;
    }

    out_samples.reserve(points.size() * 4);
    for (size_t point_index = 0; point_index + 1 < points.size(); ++point_index)
    {
        const vec3& start = points[point_index];
        const vec3& end = points[point_index + 1];
        const double distance_value = distance(start, end);

        out_samples.push_back(start);
        if (distance_value < Consts::EPS_D)
            continue;

        const double delta_t = spacing / distance_value;
        for (double t = delta_t; t < 1.0; t += delta_t)
            out_samples.emplace_back(lerp(start, end, static_cast<float>(t)));
    }

    out_samples.push_back(points.back());
}

bool SurfaceRasterizer::rasterizeSurfaceHeight(const LandscapeLayerMapPtr& terrain_tile,
                                               const ObjectSurface& object_surface,
                                               RasterBuffer& out_buffer)
{
    if (!terrain_tile)
        return false;

    out_buffer.reset(terrain_tile->getResolution());

    Vector<dvec3> vertices_world_space;
    if (!appendSurfaceTrianglesWorldSpace(object_surface, vertices_world_space))
        return false;

    const dmat4 inverse_world = terrain_tile->getIWorldTransform();
    const ivec2 resolution = terrain_tile->getResolution();
    const Vec2 size = terrain_tile->getSize();
    const Vec2 step = size / Vec2(resolution);
    const dvec2 step_d(static_cast<double>(step.x), static_cast<double>(step.y));
    bool modified = false;

    for (int vertex_index = 2; vertex_index < static_cast<int>(vertices_world_space.size()); vertex_index += 3)
    {
        // Transform in DOUBLE precision first (world coords can be huge in UNIGINE_DOUBLE mode).
        // Casting dvec3 -> Vec3 BEFORE the inverse-world multiplication silently destroys
        // precision and sends the rasterized footprint to the wrong location.
        const dvec3 lv1_d = inverse_world * vertices_world_space[vertex_index];
        const dvec3 lv2_d = inverse_world * vertices_world_space[vertex_index - 1];
        const dvec3 lv3_d = inverse_world * vertices_world_space[vertex_index - 2];

        // Down-cast to float only after we're in tile-local space (small magnitudes).
        const Vec3 local_v1(static_cast<float>(lv1_d.x), static_cast<float>(lv1_d.y), static_cast<float>(lv1_d.z));
        const Vec3 local_v2(static_cast<float>(lv2_d.x), static_cast<float>(lv2_d.y), static_cast<float>(lv2_d.z));
        const Vec3 local_v3(static_cast<float>(lv3_d.x), static_cast<float>(lv3_d.y), static_cast<float>(lv3_d.z));

        const Vec3 tri_v1(local_v1.x, local_v1.y, 0.0f);
        const Vec3 tri_v2(local_v2.x, local_v2.y, 0.0f);
        const Vec3 tri_v3(local_v3.x, local_v3.y, 0.0f);

        const int x1 = static_cast<int>(lv1_d.x / step_d.x);
        const int y1 = static_cast<int>(lv1_d.y / step_d.y);
        const int x2 = static_cast<int>(lv2_d.x / step_d.x);
        const int y2 = static_cast<int>(lv2_d.y / step_d.y);
        const int x3 = static_cast<int>(lv3_d.x / step_d.x);
        const int y3 = static_cast<int>(lv3_d.y / step_d.y);

        const int min_x = std::max(0, std::min({x1, x2, x3}));
        const int min_y = std::max(0, std::min({y1, y2, y3}));
        const int max_x = std::min(resolution.x - 1, std::max({x1, x2, x3}));
        const int max_y = std::min(resolution.y - 1, std::max({y1, y2, y3}));

        for (int x = min_x; x <= max_x; ++x)
        {
            for (int y = min_y; y <= max_y; ++y)
            {
                const int pixel_index = toIndex(x, y, resolution.x);
                if (out_buffer.source_index[pixel_index] >= 0)
                    continue;

                double bary_u = 0.0;
                double bary_v = 0.0;
                const Vec3 sample_point((x + 0.5f) * step.x, (y + 0.5f) * step.y, 0.0f);
                if (!pointInTriangle(sample_point, tri_v1, tri_v2, tri_v3, bary_u, bary_v))
                    continue;

                const Vec3 point = local_v1 +
                    ((local_v3 - local_v1) * static_cast<float>(bary_u)) +
                    ((local_v2 - local_v1) * static_cast<float>(bary_v));

                // Convert tile-local height back to world space for terrain heightmap
                const dvec3 world_point_d = terrain_tile->getWorldTransform() * dvec3(point.x, point.y, point.z);
                out_buffer.values[pixel_index] = static_cast<float>(world_point_d.z);
                out_buffer.alpha[pixel_index] = 1.0f;
                out_buffer.source_index[pixel_index] = pixel_index;
                out_buffer.seeds.push_back(pixel_index);
                modified = true;
            }
        }
    }

    return modified;
}

bool SurfaceRasterizer::rasterizeSurfaceMask(const LandscapeLayerMapPtr& terrain_tile,
                                             const ObjectSurface& object_surface,
                                             RasterBuffer& out_buffer)
{
    if (!rasterizeSurfaceHeight(terrain_tile, object_surface, out_buffer))
        return false;

    for (int pixel_index : out_buffer.seeds)
        out_buffer.values[pixel_index] = 1.0f;

    return true;
}

void SurfaceRasterizer::applyDistanceFalloff(const LandscapeLayerMapPtr& terrain_tile,
                                             RasterBuffer& buffer,
                                             double flat_distance,
                                             double falloff_distance)
{
    if (!terrain_tile || buffer.empty())
        return;

    const ivec2 resolution = terrain_tile->getResolution();
    if (resolution.x <= 0 || resolution.y <= 0)
        return;

    // Pixels-per-world-unit (use the larger axis so requested distances are at least respected).
    const Vec2 pixels_per_unit = Vec2(resolution) / Vec2(terrain_tile->getSize());
    const float ppu = std::max(pixels_per_unit.x, pixels_per_unit.y);
    const float flat_pixels = static_cast<float>(std::max(0.0, flat_distance)) * ppu;
    const float falloff_pixels = static_cast<float>(std::max(0.0, falloff_distance)) * ppu;
    const float total_pixels = flat_pixels + falloff_pixels;

    if (total_pixels <= 0.0f)
        return;

    // Chamfer 3-4 distance transform (two-pass) gives a near-Euclidean distance
    // field with smooth circular iso-lines, avoiding the diamond/octagon artifacts
    // of 4-connected BFS. We scale by 1/3 at the end so values are in pixel units.
    constexpr int kAxisCost = 3;     // cardinal neighbour
    constexpr int kDiagCost = 4;     // diagonal neighbour
    constexpr int kInfDistance = std::numeric_limits<int>::max() / 4;

    const int width = resolution.x;
    const int height = resolution.y;
    const int pixel_count = width * height;

    std::vector<int> dist(pixel_count, kInfDistance);
    std::vector<int> nearest_seed(pixel_count, -1);

    // Seed cells (rasterised triangle interior) have distance 0 and reference themselves.
    for (int seed_index : buffer.seeds)
    {
        if (seed_index >= 0 && seed_index < pixel_count)
        {
            dist[seed_index] = 0;
            nearest_seed[seed_index] = seed_index;
        }
    }

    auto relax = [&](int current_index, int neighbour_index, int cost)
    {
        const int candidate = dist[current_index] + cost;
        if (candidate < dist[neighbour_index])
        {
            dist[neighbour_index] = candidate;
            nearest_seed[neighbour_index] = nearest_seed[current_index];
        }
    };

    // Forward pass: top-left to bottom-right, look at NW/N/NE/W neighbours.
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const int idx = toIndex(x, y, width);
            if (y > 0)
            {
                if (x > 0)          relax(toIndex(x - 1, y - 1, width), idx, kDiagCost);
                                    relax(toIndex(x,     y - 1, width), idx, kAxisCost);
                if (x < width - 1)  relax(toIndex(x + 1, y - 1, width), idx, kDiagCost);
            }
            if (x > 0)              relax(toIndex(x - 1, y,     width), idx, kAxisCost);
        }
    }

    // Backward pass: bottom-right to top-left, look at SE/S/SW/E neighbours.
    for (int y = height - 1; y >= 0; --y)
    {
        for (int x = width - 1; x >= 0; --x)
        {
            const int idx = toIndex(x, y, width);
            if (y < height - 1)
            {
                if (x < width - 1)  relax(toIndex(x + 1, y + 1, width), idx, kDiagCost);
                                    relax(toIndex(x,     y + 1, width), idx, kAxisCost);
                if (x > 0)          relax(toIndex(x - 1, y + 1, width), idx, kDiagCost);
            }
            if (x < width - 1)      relax(toIndex(x + 1, y,     width), idx, kAxisCost);
        }
    }

    const auto smoothstep = [](float t) -> float
    {
        const float c = clamp(t, 0.0f, 1.0f);
        return (3.0f * c * c) - (2.0f * c * c * c);
    };

    // Convert chamfer distance to pixel-space Euclidean approximation (divide by axis cost)
    // and write height + alpha for every pixel within flat + falloff radius.
    for (int i = 0; i < pixel_count; ++i)
    {
        if (dist[i] >= kInfDistance)
            continue;

        const float pixel_dist = static_cast<float>(dist[i]) / static_cast<float>(kAxisCost);
        if (pixel_dist > total_pixels)
            continue;

        const int source = nearest_seed[i];
        if (source < 0)
            continue;

        if (buffer.source_index[i] < 0)
        {
            buffer.source_index[i] = source;
            buffer.values[i] = buffer.values[source];
        }

        if (pixel_dist <= flat_pixels || falloff_pixels <= 0.0f)
        {
            buffer.alpha[i] = 1.0f;
        }
        else
        {
            const float t = (pixel_dist - flat_pixels) / falloff_pixels;
            buffer.alpha[i] = smoothstep(1.0f - t);
        }
    }
}

void SurfaceRasterizer::blendFalloffWithExistingTerrain(const LandscapeLayerMapPtr& terrain_tile,
                                                        const LandscapeFetchPtr& fetch,
                                                        RasterBuffer& buffer)
{
    if (!terrain_tile || !fetch)
        return;
    if (buffer.resolution.x <= 0 || buffer.resolution.y <= 0)
        return;

    const dmat4 tile_world = terrain_tile->getWorldTransform();
    const Vec2 tile_size = terrain_tile->getSize();
    const ivec2 res = buffer.resolution;
    const Vec2 step = tile_size / Vec2(res);

    int falloff_pixels = 0;
    int fetch_failures = 0;

    for (int y = 0; y < res.y; ++y)
    {
        for (int x = 0; x < res.x; ++x)
        {
            const int i = toIndex(x, y, res.x);
            const float a = buffer.alpha[i];
            if (a <= 0.0f)
                continue; // pixel was not touched by the rasterizer/flood-fill

            if (a >= 1.0f)
                continue; // flat zone; height is already the final mesh height

            // Falloff ring: blend mesh height with the existing terrain height
            // at this pixel's world-space XY. If the fetch succeeds we can
            // promote the pixel to full opacity with a blended height. If it
            // fails we leave the partial alpha so the brush still feathers
            // via the opacity channel rather than producing a mesh-height cliff.
            const Vec3 local_center((x + 0.5f) * step.x, (y + 0.5f) * step.y, 0.0f);
            const dvec3 world = tile_world * dvec3(local_center.x, local_center.y, 0.0);
            const Vec2 fetch_xy(static_cast<float>(world.x), static_cast<float>(world.y));

            ++falloff_pixels;
            if (fetch->fetchForce(fetch_xy))
            {
                const float existing_h = fetch->getHeight();
                const float mesh_h = buffer.values[i];
                buffer.values[i] = existing_h + (mesh_h - existing_h) * a;
                buffer.alpha[i] = 1.0f;
            }
            else
            {
                ++fetch_failures;
            }
        }
    }

    if (falloff_pixels > 0)
    {
        Unigine::Log::message("TerrainSurfaceTool: falloff blend -> %d pixels, %d fetch failures\n",
                              falloff_pixels, fetch_failures);
    }
}

ImagePtr SurfaceRasterizer::createHeightImage(const RasterBuffer& buffer)
{
    if (buffer.resolution.x <= 0 || buffer.resolution.y <= 0)
        return nullptr;

    ImagePtr image = Image::create();
    image->create2D(buffer.resolution.x, buffer.resolution.y, Image::FORMAT_RGBA32F);

    for (int y = 0; y < buffer.resolution.y; ++y)
    {
        for (int x = 0; x < buffer.resolution.x; ++x)
        {
            const int index = toIndex(x, y, buffer.resolution.x);
            Image::Pixel pixel;
            pixel.f.r = buffer.values[index];
            pixel.f.g = buffer.values[index];
            pixel.f.b = buffer.values[index];
            pixel.f.a = buffer.alpha[index];
            image->set2D(x, y, pixel);
        }
    }

    return image;
}

ImagePtr SurfaceRasterizer::createHeightAlphaImage(const ImagePtr& height_image)
{
    if (!height_image)
        return nullptr;

    ImagePtr alpha_image = Image::create();
    alpha_image->create2D(height_image->getWidth(), height_image->getHeight(), Image::FORMAT_RGBA32F);

    for (int x = 0; x < height_image->getWidth(); ++x)
    {
        for (int y = 0; y < height_image->getHeight(); ++y)
        {
            const Image::Pixel src = height_image->get2D(x, y);
            Image::Pixel dst;
            dst.f.r = src.f.a;
            dst.f.g = src.f.a;
            dst.f.b = src.f.a;
            dst.f.a = src.f.a;
            alpha_image->set2D(x, y, dst);
        }
    }

    return alpha_image;
}

ImagePtr SurfaceRasterizer::createMaskImage(const RasterBuffer& buffer)
{
    if (buffer.resolution.x <= 0 || buffer.resolution.y <= 0)
        return nullptr;

    ImagePtr image = Image::create();
    image->create2D(buffer.resolution.x, buffer.resolution.y, Image::FORMAT_R8);

    // The landscape mask texture is sampled with Y inverted compared to the
    // height texture, so we flip Y when writing pixels to keep the mask
    // aligned with the mesh footprint in world space.
    for (int y = 0; y < buffer.resolution.y; ++y)
    {
        const int dst_y = toImageY(y, buffer.resolution.y);
        for (int x = 0; x < buffer.resolution.x; ++x)
        {
            const int index = toIndex(x, y, buffer.resolution.x);
            Image::Pixel pixel;
            pixel.i.r = clamp(static_cast<int>(buffer.alpha[index] * 255.0f), 0, 255);
            image->set2D(x, dst_y, pixel);
        }
    }

    return image;
}

int SurfaceRasterizer::toIndex(int x, int y, int width)
{
    return x + (width * y);
}

int SurfaceRasterizer::toImageY(int y, int height)
{
    return height - 1 - y;
}

bool SurfaceRasterizer::pointInTriangle(const Vec3& point,
                                        const Vec3& v1,
                                        const Vec3& v2,
                                        const Vec3& v3,
                                        double& out_u,
                                        double& out_v)
{
    const Vec3 v0 = v3 - v1;
    const Vec3 e1 = v2 - v1;
    const Vec3 e2 = point - v1;

    const double dot00 = dot(v0, v0);
    const double dot01 = dot(v0, e1);
    const double dot02 = dot(v0, e2);
    const double dot11 = dot(e1, e1);
    const double dot12 = dot(e1, e2);

    const double denominator = (dot00 * dot11) - (dot01 * dot01);
    if (std::abs(denominator) <= kEpsilon)
        return false;

    const double inverse_denominator = 1.0 / denominator;
    out_u = ((dot11 * dot02) - (dot01 * dot12)) * inverse_denominator;
    out_v = ((dot00 * dot12) - (dot01 * dot02)) * inverse_denominator;
    return (out_u >= 0.0 && out_v >= 0.0 && (out_u + out_v) <= 1.0);
}

bool SurfaceRasterizer::appendSurfaceTrianglesWorldSpace(const ObjectSurface& object_surface,
                                                         Vector<dvec3>& out_vertices)
{
    out_vertices.clear();

    if (!object_surface.first)
        return false;

    const dmat4 world_transform = object_surface.first->getWorldTransform();
    const auto object = object_surface.first;
    const int surface_index = object_surface.second;

    if (object->getType() != Node::OBJECT_MESH_STATIC)
        return false;

    ObjectMeshStaticPtr mesh_static_object = dynamic_ptr_cast<ObjectMeshStatic>(object);
    if (!mesh_static_object)
        return false;

    auto mesh = mesh_static_object->getMeshForce();
    if (!mesh)
        return false;

    const Vector<int>& indices = mesh->getCIndices(surface_index);
    const int surface_vertex_count = mesh->getNumVertices(surface_index);

    out_vertices.reserve(indices.size());
    for (int index : indices)
    {
        if (index < 0 || index >= surface_vertex_count)
            continue;

        out_vertices.push_back(world_transform * dvec3(mesh->getVertex(index, surface_index)));
    }

    return !out_vertices.empty();
}
