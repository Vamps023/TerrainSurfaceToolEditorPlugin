#include "SurfaceRasterizer.h"

#include <algorithm>
#include <cmath>
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
        collectMeshNodesRecursive(root, collected_nodes, visited_node_ids, collected_mesh_ids);

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
    bool modified = false;

    for (int vertex_index = 2; vertex_index < static_cast<int>(vertices_world_space.size()); vertex_index += 3)
    {
        const Vec3 local_v1 = inverse_world * Vec3(vertices_world_space[vertex_index]);
        const Vec3 local_v2 = inverse_world * Vec3(vertices_world_space[vertex_index - 1]);
        const Vec3 local_v3 = inverse_world * Vec3(vertices_world_space[vertex_index - 2]);

        const Vec3 tri_v1(local_v1.x, local_v1.y, 0.0f);
        const Vec3 tri_v2(local_v2.x, local_v2.y, 0.0f);
        const Vec3 tri_v3(local_v3.x, local_v3.y, 0.0f);

        const int x1 = static_cast<int>(local_v1.x / step.x);
        const int y1 = static_cast<int>(local_v1.y / step.y);
        const int x2 = static_cast<int>(local_v2.x / step.x);
        const int y2 = static_cast<int>(local_v2.y / step.y);
        const int x3 = static_cast<int>(local_v3.x / step.x);
        const int y3 = static_cast<int>(local_v3.y / step.y);

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

                out_buffer.values[pixel_index] = point.z;
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

    const Vec2 pixels_per_unit = Vec2(resolution) / Vec2(terrain_tile->getSize());
    const int flat_steps = std::max(0, ivec2(pixels_per_unit * static_cast<float>(std::max(0.0, flat_distance))).max());
    const int falloff_steps = std::max(0, ivec2(pixels_per_unit * static_cast<float>(std::max(0.0, falloff_distance))).max());
    const int total_steps = flat_steps + falloff_steps;

    if (total_steps <= 0)
        return;

    std::vector<int> frontier = buffer.seeds;
    std::vector<int> next_frontier;
    next_frontier.reserve(frontier.size() * 2);

    const auto smoothstep = [](float t) -> float
    {
        const float clamped = clamp(t, 0.0f, 1.0f);
        return (3.0f * clamped * clamped) - (2.0f * clamped * clamped * clamped);
    };

    for (int step_index = 0; step_index < total_steps && !frontier.empty(); ++step_index)
    {
        next_frontier.clear();
        for (int source_pixel_index : frontier)
        {
            const int x = source_pixel_index % resolution.x;
            const int y = source_pixel_index / resolution.x;
            const int neighbors[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};

            for (const auto& neighbor : neighbors)
            {
                const int nx = neighbor[0];
                const int ny = neighbor[1];
                if (nx < 0 || ny < 0 || nx >= resolution.x || ny >= resolution.y)
                    continue;

                const int neighbor_index = toIndex(nx, ny, resolution.x);
                if (buffer.source_index[neighbor_index] >= 0)
                    continue;

                const int root_index = buffer.source_index[source_pixel_index] >= 0
                    ? buffer.source_index[source_pixel_index]
                    : source_pixel_index;

                buffer.source_index[neighbor_index] = root_index;
                buffer.values[neighbor_index] = buffer.values[root_index];

                const int distance_step = step_index + 1;
                if (distance_step <= flat_steps)
                {
                    buffer.alpha[neighbor_index] = 1.0f;
                }
                else if (falloff_steps > 0)
                {
                    const float t = static_cast<float>(total_steps - distance_step) /
                                    static_cast<float>(falloff_steps);
                    buffer.alpha[neighbor_index] = smoothstep(t);
                }
                else
                {
                    buffer.alpha[neighbor_index] = 1.0f;
                }

                next_frontier.push_back(neighbor_index);
            }
        }

        frontier.swap(next_frontier);
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
        const int image_y = toImageY(y, buffer.resolution.y);
        for (int x = 0; x < buffer.resolution.x; ++x)
        {
            const int index = toIndex(x, y, buffer.resolution.x);
            Image::Pixel pixel;
            pixel.f.r = buffer.values[index];
            pixel.f.g = buffer.values[index];
            pixel.f.b = buffer.values[index];
            pixel.f.a = buffer.alpha[index];
            image->set2D(x, image_y, pixel);
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

    for (int y = 0; y < buffer.resolution.y; ++y)
    {
        const int image_y = toImageY(y, buffer.resolution.y);
        for (int x = 0; x < buffer.resolution.x; ++x)
        {
            const int index = toIndex(x, y, buffer.resolution.x);
            Image::Pixel pixel;
            pixel.i.r = clamp(static_cast<int>(buffer.alpha[index] * 255.0f), 0, 255);
            image->set2D(x, image_y, pixel);
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

void SurfaceRasterizer::collectMeshNodesRecursive(const NodePtr& node,
                                                  std::vector<NodePtr>& out_nodes,
                                                  std::unordered_set<int>& visited_node_ids,
                                                  std::unordered_set<int>& collected_mesh_ids)
{
    if (!node)
        return;

    const int node_id = node->getID();
    if (!visited_node_ids.insert(node_id).second)
        return;

    if (node->getType() == Node::OBJECT_MESH_STATIC || node->getType() == Node::OBJECT_MESH_DYNAMIC)
    {
        if (collected_mesh_ids.insert(node_id).second)
            out_nodes.push_back(node);
    }
    else if (node->getType() == Node::NODE_REFERENCE)
    {
        auto reference = checked_ptr_cast<NodeReference>(node);
        if (reference)
        {
            auto target = reference->getReference();
            if (target)
                collectMeshNodesRecursive(target, out_nodes, visited_node_ids, collected_mesh_ids);
        }
    }

    for (int child_index = 0; child_index < node->getNumChildren(); ++child_index)
        collectMeshNodesRecursive(node->getChild(child_index), out_nodes, visited_node_ids, collected_mesh_ids);
}
