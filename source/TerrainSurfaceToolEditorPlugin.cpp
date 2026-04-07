/* ========================================================================
 * TerrainSurfaceToolEditorPlugin.cpp
 *
 * Pure Editor2 Plugin - "Pull Terrain To Surface"
 * UI pattern: docked QWidget panel (same as GantryLabelTool)
 * Unigine 2.18 - Real Landscape Mask Support
 * ======================================================================== */

#define _USE_MATH_DEFINES
#include "TerrainSurfaceToolEditorPlugin.h"
#include <UnigineCallback.h>
#include <UnigineRender.h>
#include <math.h>
#include <QtWidgets/QApplication>
#include <QtGui/QPalette>

using namespace Unigine;
using namespace Unigine::Math;

namespace
{
int getLandscapeMaskFlag(int mask_index)
{
    if (mask_index < 0 || mask_index > 19)
        return 0;
    return Landscape::FLAGS_DATA_MASK_0 << mask_index;
}

int round_up(int value, int round)
{
    return ((value + (round - 1)) / round) * round;
}

ivec2 round_up(ivec2 value, ivec2 round)
{
    return ((value + (round - 1)) / round) * round;
}

int group_threads(int width, int kernel_size)
{
    return __max(1, round_up(width, kernel_size) / kernel_size);
}

ImagePtr createHeightAlphaImage(const ImagePtr &height_image)
{
    if (!height_image)
        return nullptr;

    ImagePtr alpha_image = Image::create();
    alpha_image->create2D(height_image->getWidth(), height_image->getHeight(), Image::FORMAT_RGBA32F);

    for (int x = 0; x < height_image->getWidth(); ++x)
    {
        for (int y = 0; y < height_image->getHeight(); ++y)
        {
            Image::Pixel src = height_image->get2D(x, y);
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
}

/* ========================================================================
 * TerrainManipulator implementation
 * ======================================================================== */

TerrainManipulator::TerrainManipulator()
{
    m_event_connection_id = Landscape::getEventTextureDraw().connect(
        [this](const UGUID& guid, int id, const LandscapeTexturesPtr& buffer,
            const Math::ivec2& coord, int data_mask)
        { OnTextureDraw(guid, id, buffer, coord, data_mask); });
}

TerrainManipulator::~TerrainManipulator()
{
    if (m_event_connection_id)
    {
        Landscape::getEventTextureDraw().disconnect(m_event_connection_id);
        m_event_connection_id = nullptr;
    }
}

bool TerrainManipulator::IsTerrainManipulationInProgress() const
{
    return m_in_progress;
}

size_t TerrainManipulator::NumberOfRemainingOperations() const
{
    return m_operations_blocked ? m_pending_operations.size() + m_lock_count
                                : m_pending_operations.size();
}

void TerrainManipulator::BlockBrushOperations(bool block)
{
    m_operations_blocked = block;
}

void TerrainManipulator::SetBlockOperationCount(int count)
{
    m_lock_count = count;
    m_operations_blocked = m_lock_count > 0;
}

/* --- RaiseTerrainToSurface ---
 * The main entry point: given a node and surface name, extract mesh vertices
 * in world space, compute midpoints, sample along the path, then raise terrain
 * at each sampled point. */
void TerrainManipulator::RaiseTerrainToSurface(
    NodePtr node, const std::string& surface_name,
    dvec2 brush_size, MaterialPtr brush_material, bool target_albedo)
{
    if (node->getType() != Node::OBJECT_MESH_STATIC)
    {
        Log::warning("RaiseTerrainToSurface: node is not OBJECT_MESH_STATIC\n");
        return;
    }

    auto static_mesh = static_ptr_cast<ObjectMeshStatic>(node);
    const auto surface_verts = GetSurfaceVerticesFromMeshInWorldSpace(static_mesh, surface_name);

    if (surface_verts.size() < 2)
    {
        Log::warning("RaiseTerrainToSurface: not enough vertices (%d) on surface '%s'\n",
                     (int)surface_verts.size(), surface_name.c_str());
        return;
    }

    Log::message("RaiseTerrainToSurface: found %d vertices on surface '%s'\n",
                 (int)surface_verts.size(), surface_name.c_str());

    // Compute midpoints between consecutive vertex pairs
    std::vector<vec3> mid_points;
    for (int i = 0; i < (int)surface_verts.size() - 1; i += 2)
    {
        auto p1 = surface_verts[i];
        auto p2 = surface_verts[i + 1];
        float midX = (p1.x + p2.x) / 2.0f;
        float midY = (p1.y + p2.y) / 2.0f;
        float midZ = (p1.z + p2.z) / 2.0f;
        mid_points.push_back(vec3(midX, midY, midZ));
    }

    if (mid_points.empty())
        return;

    // Sample along the midpoint path at brush_size intervals for smooth coverage
    std::vector<vec3> brush_locales;
    const double sample_spacing = std::clamp(static_cast<double>(brush_size.x) * 0.2, 1.0, 10.0);
    for (int i = 0; i < (int)mid_points.size() - 1; i++)
    {
        auto samples = SampleLine(mid_points[i], mid_points[i + 1], sample_spacing);
        brush_locales.push_back(mid_points[i]);
        brush_locales.insert(brush_locales.end(), samples.begin(), samples.end());
        brush_locales.push_back(mid_points[i + 1]);
    }
    // If only one midpoint, use it directly
    if (mid_points.size() == 1)
        brush_locales.push_back(mid_points[0]);

    // Calculate brush rotation from direction between consecutive points
    auto calculate_direction = [](vec3 x1, vec3 x2) -> double
    {
        double delta_x = x2.x - x1.x;
        double delta_y = x2.y - x1.y;
        double angle_rad = std::atan2(delta_y, delta_x);
        double angle_deg = angle_rad * 180.0 / M_PI;
        return angle_deg;
    };

    float brush_rotation = 0;
    for (int i = 0; i < (int)brush_locales.size(); i++)
    {
        auto v = brush_locales[i];

        if (i < (int)brush_locales.size() - 1)
        {
            auto v1 = brush_locales[i + 1];
            brush_rotation = static_cast<float>(calculate_direction(v, v1) + 90.0);
        }

        RaiseTerrainToVertex(dvec3(v.x, v.y, v.z), brush_size, brush_material, brush_rotation, nullptr, target_albedo);
    }

    Log::message("RaiseTerrainToSurface: queued %d brush operations\n", (int)brush_locales.size());
}

/* --- RaiseTerrainToVertices --- */
void TerrainManipulator::RaiseTerrainToVertices(
    std::vector<dvec3> vertices, dvec2 brush_size, MaterialPtr brush_material)
{
    auto calculate_direction = [](dvec3 x1, dvec3 x2) -> double
    {
        double delta_x = x2.x - x1.x;
        double delta_y = x2.y - x1.y;
        return std::atan2(delta_y, delta_x) * 180.0 / M_PI;
    };

    double brush_rotation = 0;
    for (int i = 0; i < (int)vertices.size(); i++)
    {
        if (i < (int)vertices.size() - 1)
            brush_rotation = calculate_direction(vertices[i], vertices[i + 1]) + 90.0;

        RaiseTerrainToVertex(vertices[i], brush_size, brush_material,
                             static_cast<float>(brush_rotation));
    }
}

/* --- RaiseTerrainToBoundingBox --- */
void TerrainManipulator::RaiseTerrainToBoundingBox(
    NodePtr node, dvec2 brush_size, MaterialPtr brush_material)
{
    auto bb = node->getHierarchyWorldBoundBox();
    auto center = bb.getCenter();
    center.z = bb.minimum.z;
    RaiseTerrainToVertex(center, brush_size, brush_material, 0);
}

/* --- RaiseTerrainToVertex ---
 * Core per-vertex terrain modification. Finds the closest LandscapeLayerMap,
 * calculates the drawing region, queues a brush operation, and triggers
 * an async texture draw. */
void TerrainManipulator::RaiseTerrainToVertex(
    dvec3 vertex, dvec2 brush_size, MaterialPtr brush_material,
    float brush_rotation, LandscapeLayerMapPtr map_to_operate, bool target_albedo)
{
    if (!TerrainTestFetchAtPosition(dvec2{ vertex.x, vertex.y }))
        return;

    ObjectLandscapeTerrainPtr terrain{ Landscape::getActiveTerrain() };
    const auto d_position = dvec3(vertex);

    auto closest_lmap = GetClosestLandscapeLayerMap(terrain, d_position);
    if (map_to_operate)
        closest_lmap = map_to_operate;
    if (!closest_lmap)
        return;

    // Convert world position to the layer map's local coordinate space
    auto brush_local_position = closest_lmap->getIWorldTransform() * d_position;

    quat brush_world_rotation = quat(vec3_up, brush_rotation);
    quat brush_local_rotation = brush_world_rotation * inverse(closest_lmap->getWorldRotation());

    auto half_size = brush_size / 2.0f;

    ivec2 drawing_region_coord;
    ivec2 drawing_region_size;
    CalculateDrawingRegion(brush_local_position, brush_local_rotation, half_size,
                           closest_lmap, drawing_region_coord, drawing_region_size);

    auto pixels_per_unit = Vec2{ closest_lmap->getResolution() } / Vec2{ closest_lmap->getSize() };
    auto pixel_coord = ivec2{ pixels_per_unit * Vec2(brush_local_position) };

    Log::message("raising at tile loc (%g, %g) pixel (%d, %d) draw (%d, %d) size (%d %d)\n",
        brush_local_position.x, brush_local_position.y,
        pixel_coord.x, pixel_coord.y,
        drawing_region_coord.x, drawing_region_coord.y,
        drawing_region_size.x, drawing_region_size.y);

    // Queue the brush operation
    BrushOperationData data;
    data.brush_height = static_cast<float>(vertex.z);
    data.brush_size = static_cast<float>(max(drawing_region_size.x, drawing_region_size.y));
    data.brush_opacity = 1;
    data.height_blend_mode = HeightBlendMode::Alpha;
    data.brush_material = brush_material;
    data.brush_rotation = brush_rotation;
    data.modify_heights = !target_albedo;
    data.modify_albedo = target_albedo;
    data.modify_mask = false;
    data.mask_index = 0;

    int operation_id = Landscape::generateOperationID();
    m_pending_operations.emplace(operation_id, data);
    Landscape::asyncTextureDraw(operation_id, closest_lmap->getGUID(), drawing_region_coord,
                                drawing_region_size,
                                Landscape::FLAGS_DATA_HEIGHT | Landscape::FLAGS_DATA_ALBEDO);

    // Handle brush operations that overfill a terrain chunk boundary
    auto max_x = closest_lmap->getSize().x - drawing_region_size.x;
    auto max_y = closest_lmap->getSize().y - drawing_region_size.y;
    if ((drawing_region_coord.x <= 0 || drawing_region_coord.x >= max_x ||
         drawing_region_coord.y <= 0 || drawing_region_coord.y >= max_y) &&
        map_to_operate == nullptr)
    {
        auto current_pos = closest_lmap->getWorldPosition();
        auto dws = dvec3();
        dws.x = drawing_region_coord.x <= 0
            ? current_pos.x + drawing_region_coord.x
            : current_pos.x + drawing_region_coord.x + drawing_region_size.x;
        dws.y = drawing_region_coord.y <= 0
            ? current_pos.y + drawing_region_coord.y
            : current_pos.y + drawing_region_coord.y + drawing_region_size.y;
        dws.z = d_position.z;

        auto op_lmap = GetClosestLandscapeLayerMap(terrain, dws);
        if (op_lmap == closest_lmap || map_to_operate == op_lmap)
            return;

        RaiseTerrainToVertex(vertex, brush_size, brush_material, brush_rotation, op_lmap, target_albedo);
    }
}

/* --- SetTerrainHeight --- */
bool TerrainManipulator::SetTerrainHeight(LandscapeLayerMapPtr lmap, ImagePtr height_image)
{
    if (!lmap || !height_image)
        return false;

    if (height_image->getFormat() != Image::FORMAT_RGBA32F)
    {
        if (!height_image->convertToFormat(Image::FORMAT_RGBA32F))
        {
            Log::warning("[TerrainManipulator] Failed to convert height image to RGBA32F.\n");
            return false;
        }
    }

    const ivec2 tile_resolution = lmap->getResolution();
    if (height_image->getWidth() != tile_resolution.x || height_image->getHeight() != tile_resolution.y)
    {
        if (!height_image->resize(tile_resolution.x, tile_resolution.y))
        {
            Log::warning("[TerrainManipulator] Failed to resize height image to tile resolution (%d x %d).\n",
                         tile_resolution.x, tile_resolution.y);
            return false;
        }
    }

    auto alpha_image = createHeightAlphaImage(height_image);
    if (!alpha_image)
        return false;

    BrushOperationData data;
    data.brush_height = 1;
    data.brush_size = 1;
    data.brush_rotation = 0;
    data.brush_opacity = 1;
    data.height_blend_mode = HeightBlendMode::Alpha;
    data.modify_heights = true;
    data.modify_albedo = false;
    data.modify_mask = false;
    data.mask_index = 0;
    data.height_image = height_image;
    data.alpha_image = alpha_image;

    int operation_id = Landscape::generateOperationID();
    m_pending_operations.emplace(operation_id, data);
    Landscape::asyncTextureDraw(operation_id, lmap->getGUID(), ivec2{ 0, 0 }, lmap->getResolution(),
                                Landscape::FLAGS_FILE_DATA_HEIGHT |
                                Landscape::FLAGS_FILE_DATA_OPACITY_HEIGHT);
    return true;
}

void TerrainManipulator::SetTerrainHeightImmediate(LandscapeLayerMapPtr lmap, UGUID guid, int id,
                                                   LandscapeTexturesPtr buffer, ivec2 coord,
                                                   int data_mask, TexturePtr height_texture,
                                                   bool overwrite_with_32bit_float_data)
{
    SetTerrainHeightImmediate(lmap, guid, id, buffer, coord, data_mask,
                              height_texture, buffer ? buffer->getOpacityHeight() : nullptr,
                              overwrite_with_32bit_float_data);
}

void TerrainManipulator::SetTerrainHeightImmediate(LandscapeLayerMapPtr lmap, UGUID guid, int id,
                                                   LandscapeTexturesPtr buffer, ivec2 coord,
                                                   int data_mask, TexturePtr height_texture,
                                                   TexturePtr alpha_texture,
                                                   bool overwrite_with_32bit_float_data)
{
    UNIGINE_UNUSED(lmap);
    UNIGINE_UNUSED(guid);
    UNIGINE_UNUSED(id);
    UNIGINE_UNUSED(coord);
    UNIGINE_UNUSED(data_mask);

    if (!buffer || !height_texture || !alpha_texture)
        return;

    if (!overwrite_with_32bit_float_data)
    {
        Log::warning("[TerrainManipulator] Non-overwrite immediate terrain writes are not implemented.\n");
        return;
    }

    std::string base_brush_mat_name = "terrain_brush_r32f_overwrite.basebrush";
    auto file_guid = FileSystem::getGUID(FileSystem::resolvePartialVirtualPath(base_brush_mat_name.c_str()));
    if (!file_guid.isValid())
    {
        Log::warning("[TerrainManipulator] Could not find brush material: %s\n", base_brush_mat_name.c_str());
        return;
    }

    auto brush_material = Materials::findMaterialByFileGUID(file_guid)->inherit();
    brush_material->setTexture("terrain_height", buffer->getHeight());
    brush_material->setTexture("terrain_opacity_height", buffer->getOpacityHeight());
    brush_material->setTexture("new_height", height_texture);
    brush_material->setTexture("new_alpha", alpha_texture);
    brush_material->runExpression("brush", buffer->getResolution().x, buffer->getResolution().y);
    brush_material->setTexture("terrain_height", nullptr);
    brush_material->setTexture("terrain_opacity_height", nullptr);
    brush_material->setTexture("new_height", nullptr);
    brush_material->setTexture("new_alpha", nullptr);
}

bool TerrainManipulator::RaiseTerrainToSurfacesCompute(const std::vector<NodePtr>& nodes,
                                                       const std::string& surface_pattern,
                                                       double flat_distance, double falloff_distance,
                                                       double smoothing_strength, double bias)
{
    const char* compute_material_name = "raise_terrain_to_surfaces_comp.basemat";
    auto file_guid = FileSystem::getGUID(FileSystem::resolvePartialVirtualPath(compute_material_name));
    if (!file_guid.isValid())
    {
        Log::warning("[TerrainManipulator] Could not find terrain compute material: %s\n",
                     compute_material_name);
        return false;
    }

    auto compute_material = Materials::findMaterialByFileGUID(file_guid)->inherit();
    auto active_terrain = Landscape::getActiveTerrain();
    if (!active_terrain)
    {
        Log::warning("[TerrainManipulator] No active terrain found for compute sculpt.\n");
        return false;
    }

    std::regex surface_regex;
    try
    {
        surface_regex = std::regex(surface_pattern);
    }
    catch (const std::regex_error& e)
    {
        Log::warning("[TerrainManipulator] Invalid surface regex '%s': %s\n",
                     surface_pattern.c_str(), e.what());
        return false;
    }

    Unigine::Vector<NodePtr> selected_nodes;
    for (const auto& node : nodes)
        selected_nodes.append(node);

    auto flattened_objects = flattenToObjectList(selected_nodes);
    std::vector<ObjectSurface> candidate_pairs;
    candidate_pairs.reserve(flattened_objects.size());
    for (auto obj : flattened_objects)
    {
        auto surface = getBaseSurface(obj, surface_regex);
        if (surface.first)
            candidate_pairs.push_back(surface);
    }

    if (candidate_pairs.empty())
    {
        Log::warning("[TerrainManipulator] No surfaces matched '%s' for compute sculpt.\n",
                     surface_pattern.c_str());
        return false;
    }

    std::vector<LandscapeLayerMapPtr> terrain_tiles;
    terrain_tiles.reserve(active_terrain->getNumChildren());
    for (int i = 0; i < active_terrain->getNumChildren(); ++i)
    {
        if (auto lmap = checked_ptr_cast<LandscapeLayerMap>(active_terrain->getChild(i)); lmap != nullptr)
            terrain_tiles.push_back(lmap);
    }

    if (terrain_tiles.empty())
    {
        Log::warning("[TerrainManipulator] No landscape layer maps found for compute sculpt.\n");
        return false;
    }

    bool scheduled_any = false;
    const float radius = static_cast<float>(flat_distance + falloff_distance);

    for (auto terrain_tile : terrain_tiles)
    {
        auto objects_for_tile = getObjectsWithinBoundingBox(
            terrain_tile->getWorldBoundBox(), candidate_pairs, radius);
        if (objects_for_tile.empty())
            continue;

        auto terrain_guid = terrain_tile->getGUID();
        auto tile_resolution = terrain_tile->getResolution();
        if (tile_resolution.x <= 0 || tile_resolution.y <= 0)
            continue;

        ivec2 max_subtile_resolution = ivec2(1024);
        ivec2 tile_subtiles = round_up(tile_resolution, max_subtile_resolution) / max_subtile_resolution;

        for (int subtile_y = 0; subtile_y < tile_subtiles.y; ++subtile_y)
        {
            for (int subtile_x = 0; subtile_x < tile_subtiles.x; ++subtile_x)
            {
                ivec2 subtile_pixel_coord = ivec2(subtile_x, subtile_y) * max_subtile_resolution;
                ivec2 subtile_resolution = min(tile_resolution - subtile_pixel_coord, max_subtile_resolution);

                BoundBox tile_bb = terrain_tile->getBoundBox();
                dmat4 tile_world_transform = terrain_tile->getWorldTransform();
                vec3 subtile_minimum(vec2(subtile_pixel_coord) * vec2(terrain_tile->getTexelSize()),
                                     tile_bb.minimum.z);
                vec3 subtile_maximum(vec2(subtile_pixel_coord + subtile_resolution) * vec2(terrain_tile->getTexelSize()),
                                     tile_bb.maximum.z);
                BoundBox subtile_bb(subtile_minimum, subtile_maximum);
                WorldBoundBox subtile_wbb = WorldBoundBox(subtile_bb, tile_world_transform);

                auto objects_for_subtile = getObjectsWithinBoundingBox(subtile_wbb, objects_for_tile, radius);
                if (objects_for_subtile.empty())
                    continue;

                int operation_id = Landscape::generateOperationID();
                m_async_operations_count++;
                SetBlockOperationCount(m_async_operations_count);

                EventConnections* connections = new EventConnections();
                Landscape::getEventTextureDraw().connect(
                    *connections,
                    [this,
                     subtile_pixel_coord,
                     subtile_resolution,
                     compute_material,
                     connections,
                     terrain_guid,
                     operation_id,
                     terrain_tile,
                     objects_for_subtile,
                     flat_distance,
                     falloff_distance,
                     bias,
                     smoothing_strength](const UGUID& invoke_guid, int invoke_id,
                                         const LandscapeTexturesPtr& buffer,
                                         const ivec2& coord, int data_mask)
                    {
                        if (invoke_guid != terrain_guid || invoke_id != operation_id)
                            return;

                        const ivec2 buffer_resolution = buffer ? buffer->getResolution() : ivec2_zero;
                        const bool has_height_texture = buffer && buffer->getHeight().isValid();
                        const bool has_opacity_texture = buffer && buffer->getOpacityHeight().isValid();
                        Log::message("[TerrainManipulator] Compute callback for tile '%s' subtile (%d,%d) size (%d,%d), buffer (%d,%d), coord (%d,%d), data_mask=0x%X, height=%d, opacity=%d\n",
                                     terrain_tile->getName(),
                                     subtile_pixel_coord.x, subtile_pixel_coord.y,
                                     subtile_resolution.x, subtile_resolution.y,
                                     buffer_resolution.x, buffer_resolution.y,
                                     coord.x, coord.y, data_mask,
                                     has_height_texture ? 1 : 0,
                                     has_opacity_texture ? 1 : 0);

                        auto finish_operation = [this, connections]()
                        {
                            m_async_operations_count--;
                            SetBlockOperationCount(m_async_operations_count);
                            delete connections;
                        };

                        TexturePtr output_texture_gpu = Texture::create();
                        if (!output_texture_gpu.isValid())
                        {
                            finish_operation();
                            return;
                        }
                        output_texture_gpu->create2D(
                            subtile_resolution.x,
                            subtile_resolution.y,
                            Texture::FORMAT_R32F,
                            Texture::FORMAT_USAGE_RENDER | Texture::FORMAT_USAGE_UNORDERED_ACCESS);
                        if (!output_texture_gpu->isValid())
                        {
                            finish_operation();
                            return;
                        }

                        TexturePtr output_alpha_texture_gpu = Texture::create();
                        if (!output_alpha_texture_gpu.isValid())
                        {
                            finish_operation();
                            return;
                        }
                        output_alpha_texture_gpu->create2D(
                            subtile_resolution.x,
                            subtile_resolution.y,
                            Texture::FORMAT_R32F,
                            Texture::FORMAT_USAGE_RENDER | Texture::FORMAT_USAGE_UNORDERED_ACCESS);
                        if (!output_alpha_texture_gpu->isValid())
                        {
                            finish_operation();
                            return;
                        }

                        TexturePtr src_height_texture = buffer->getHeight();
                        TexturePtr src_opacity_texture = buffer->getOpacityHeight();
                        if (!src_height_texture.isValid() || !src_height_texture->isValid() ||
                            !src_opacity_texture.isValid() || !src_opacity_texture->isValid())
                        {
                            finish_operation();
                            return;
                        }

                        Unigine::Vector<dvec3> vertices_world_space;
                        for (const auto& object_surface : objects_for_subtile)
                            appendBaseVerts(object_surface, vertices_world_space);

                        const size_t z_vertex_count = vertices_world_space.size();
                        Log::message("[TerrainManipulator] Compute input vertex count: %d\n",
                                     static_cast<int>(z_vertex_count));
                        if (z_vertex_count == 0 || (z_vertex_count % 3) != 0 || z_vertex_count >= INT_MAX)
                        {
                            finish_operation();
                            return;
                        }

                        StructuredBufferPtr vertices_world_space_buffer_gpu = StructuredBuffer::create();
                        vertices_world_space_buffer_gpu->create(
                            StructuredBuffer::GPU_RESOURCE | StructuredBuffer::IMMUTABLE,
                            vertices_world_space.get(),
                            sizeof(dvec3),
                            static_cast<unsigned int>(z_vertex_count));
                        if (!vertices_world_space_buffer_gpu.isValid() ||
                            !vertices_world_space_buffer_gpu->isValid())
                        {
                            finish_operation();
                            return;
                        }

                        struct ConfigBuffer
                        {
                            dmat4x4_values tile_world_transform;
                            dmat4x4_values tile_inv_world_transform;
                            unsigned int triangle_count;
                            unsigned int vertex_count;
                            ivec2 tile_resolution;
                            ivec2 subtile_pixel_coord;
                            ivec2 subtile_resolution;
                            float flat_distance;
                            float falloff_distance;
                            float bias;
                            float rational;
                            vec2 tile_size;
                            vec2 tile_coord_to_position;
                        };

                        ConfigBuffer config = {};
                        config.flat_distance = static_cast<float>(flat_distance);
                        config.falloff_distance = static_cast<float>(falloff_distance);
                        config.bias = static_cast<float>(bias);
                        config.rational = 0.0f;
                        if (smoothing_strength > 0.0)
                        {
                            config.rational = 1.0f / clamp(
                                1.0f - static_cast<float>(smoothing_strength), 0.01f, 0.99f);
                        }

                        config.vertex_count = static_cast<int>(z_vertex_count);
                        config.triangle_count = config.vertex_count / 3;
                        config.tile_resolution = terrain_tile->getResolution();
                        config.subtile_pixel_coord = subtile_pixel_coord;
                        config.subtile_resolution = subtile_resolution;
                        config.tile_size = vec2(terrain_tile->getSize());
                        config.tile_coord_to_position = vec2(terrain_tile->getTexelSize());

                        dmat4 terrain_world_transform = terrain_tile->getWorldTransform();
                        terrain_world_transform.get(config.tile_world_transform, true);
                        dmat4 terrain_inv_world_transform = terrain_tile->getIWorldTransform();
                        terrain_inv_world_transform.get(config.tile_inv_world_transform, true);

                        StructuredBufferPtr config_buffer_gpu = StructuredBuffer::create();
                        config_buffer_gpu->create(
                            StructuredBuffer::GPU_RESOURCE | StructuredBuffer::IMMUTABLE,
                            &config,
                            sizeof(ConfigBuffer),
                            1);
                        if (!config_buffer_gpu.isValid() || !config_buffer_gpu->isValid())
                        {
                            finish_operation();
                            return;
                        }

                        RenderState::setTexture(RenderState::BIND_ALL, 0, src_height_texture);
                        RenderState::setTexture(RenderState::BIND_ALL, 1, src_opacity_texture);

                        RenderTargetPtr render_target = RenderTarget::create();
                        if (!render_target.isValid())
                        {
                            finish_operation();
                            return;
                        }

                        render_target->bindStructuredBuffer(0, config_buffer_gpu);
                        render_target->bindUnorderedAccessTexture(1, output_texture_gpu, true);
                        render_target->bindUnorderedAccessTexture(2, output_alpha_texture_gpu, true);
                        render_target->bindStructuredBuffer(3, vertices_world_space_buffer_gpu);
                        render_target->enableCompute();

                        constexpr int NUM_GROUP_X = 32;
                        constexpr int NUM_GROUP_Y = 32;
                        int group_threads_x = group_threads(subtile_resolution.x, NUM_GROUP_X);
                        int group_threads_y = group_threads(subtile_resolution.y, NUM_GROUP_Y);
                        constexpr int thread_group_block = 16;

                        for (int thread_offset_y = 0; thread_offset_y < group_threads_y; thread_offset_y += thread_group_block)
                        {
                            int threads_lower_y = thread_offset_y;
                            int threads_upper_y = min(group_threads_y, thread_offset_y + thread_group_block);
                            int threads_y = threads_upper_y - threads_lower_y;

                            for (int thread_offset_x = 0; thread_offset_x < group_threads_x; thread_offset_x += thread_group_block)
                            {
                                int threads_lower_x = thread_offset_x;
                                int threads_upper_x = min(group_threads_x, thread_offset_x + thread_group_block);
                                int threads_x = threads_upper_x - threads_lower_x;

                                struct GroupBuffer
                                {
                                    int offset_x;
                                    int offset_y;
                                    int threads_x;
                                    int threads_y;
                                    int threads_lower_x;
                                    int threads_upper_x;
                                    int threads_lower_y;
                                    int threads_upper_y;
                                };

                                GroupBuffer group = {};
                                group.offset_x = threads_lower_x * NUM_GROUP_X;
                                group.offset_y = threads_lower_y * NUM_GROUP_Y;
                                group.threads_x = threads_x;
                                group.threads_y = threads_y;
                                group.threads_lower_x = threads_lower_x;
                                group.threads_upper_x = threads_upper_x;
                                group.threads_lower_y = threads_lower_y;
                                group.threads_upper_y = threads_upper_y;

                                StructuredBufferPtr group_buffer_gpu = StructuredBuffer::create();
                                group_buffer_gpu->create(
                                    StructuredBuffer::GPU_RESOURCE | StructuredBuffer::IMMUTABLE,
                                    &group,
                                    sizeof(GroupBuffer),
                                    1);
                                if (!group_buffer_gpu.isValid() || !group_buffer_gpu->isValid())
                                    continue;

                                render_target->bindStructuredBuffer(4, group_buffer_gpu);
                                compute_material->renderCompute(Render::PASS_POST, threads_x, threads_y);
                                render_target->flush();
                            }
                        }

                        render_target->disable();
                        render_target->unbindAll();
                        RenderState::setTexture(RenderState::BIND_ALL, 0, nullptr);
                        RenderState::setTexture(RenderState::BIND_ALL, 1, nullptr);

                        SetTerrainHeightImmediate(
                            terrain_tile, invoke_guid, invoke_id, buffer, coord, data_mask,
                            output_texture_gpu, output_alpha_texture_gpu, true);
                        Log::message("[TerrainManipulator] Compute height writeback complete for tile '%s'\n",
                                     terrain_tile->getName());

                        finish_operation();
                    });

                const int compute_data_flags =
                    Landscape::FLAGS_FILE_DATA_HEIGHT |
                    Landscape::FLAGS_FILE_DATA_OPACITY_HEIGHT;
                Landscape::asyncTextureDraw(
                    operation_id, terrain_guid, subtile_pixel_coord, subtile_resolution,
                    compute_data_flags);
                Log::message("[TerrainManipulator] Queued compute sculpt for tile '%s' subtile (%d,%d) size (%d,%d)\n",
                             terrain_tile->getName(),
                             subtile_pixel_coord.x, subtile_pixel_coord.y,
                             subtile_resolution.x, subtile_resolution.y);
                scheduled_any = true;
            }
        }
    }

    if (!scheduled_any)
    {
        Log::warning("[TerrainManipulator] Compute sculpt found no terrain subtile intersections.\n");
    }

    return scheduled_any;
}

/* --- SetTerrainAlbedoImmediate ---
 * Direct albedo texture overwrite using terrain_brush_r32f_overwrite.basebrush pattern
 * This bypasses the UV-based sampling of brush.basebrush for direct pixel mapping */
void TerrainManipulator::SetTerrainAlbedoImmediate(LandscapeLayerMapPtr lmap, UGUID guid, int id,
                                                   LandscapeTexturesPtr buffer, ivec2 coord,
                                                   int data_mask, ImagePtr albedo_image)
{
    std::string base_brush_mat_name = "terrain_brush_r32f_overwrite.basebrush";

    auto file_guid = FileSystem::getGUID(FileSystem::resolvePartialVirtualPath(base_brush_mat_name.c_str()));
    if (!file_guid.isValid())
    {
        Log::warning("[TerrainManipulator] Could not find brush material: %s\n", base_brush_mat_name.c_str());
        return;
    }

    auto brush_material = Materials::findMaterialByFileGUID(file_guid)->inherit();

    auto albedo_texture = Texture::create();
    albedo_texture->setImage(albedo_image);

    brush_material->setTexture("terrain_height", buffer->getAlbedo());
    brush_material->setTexture("new_height", albedo_texture);

    brush_material->runExpression("brush", buffer->getResolution().x, buffer->getResolution().y);

    brush_material->setTexture("terrain_height", nullptr);
    brush_material->setTexture("new_height", nullptr);
}

/* ========================================================================
 * SetTerrainMask  (Unigine 2.18 - Real Landscape Mask Channel)
 *
 * Writes to the actual landscape mask slot (mask_index) via getMask(index)
 * on the LandscapeTextures buffer, using FLAGS_DATA_MASKS so Unigine
 * populates those slots in the async callback. The mask image should be
 * grayscale (R8): white = fully masked, black = no mask.
 * ======================================================================== */
bool TerrainManipulator::SetTerrainMask(
    LandscapeLayerMapPtr lmap, NodePtr node,
    ImagePtr mask_image, dvec2 brush_size, int mask_index)
{
    if (!lmap || !mask_image || !node)
        return false;

    auto tile_res = lmap->getResolution();

    // Resize mask image to match tile resolution if needed
    if (mask_image->getWidth() != tile_res.x || mask_image->getHeight() != tile_res.y)
    {
        Log::message("[TerrainManipulator] Resizing mask image to tile resolution (%d x %d)\n",
                     tile_res.x, tile_res.y);
        mask_image->resize(tile_res.x, tile_res.y);
    }

    // Unigine 2.18 landscape masks use R8 single-channel grayscale format
    // White (255) = fully masked, Black (0) = no mask
    if (mask_image->getFormat() != Image::FORMAT_R8)
    {
        Log::message("[TerrainManipulator] Converting mask image to R8 format\n");
        mask_image->convertToFormat(Image::FORMAT_R8);
    }

    // Create brush material using mask image as the opacity texture
    auto brush = getMaskTerrainBrush(mask_image);
    if (!brush)
    {
        Log::warning("[TerrainManipulator] Could not create brush for mask painting\n");
        return false;
    }

    Log::message("[TerrainManipulator] Mask painting: tile_res (%d x %d), mask_index (%d)\n",
                 tile_res.x, tile_res.y, mask_index);

    // Queue brush operation targeting the real landscape mask channel
    BrushOperationData data;
    data.brush_material    = brush;
    data.brush_height      = 1;
    data.brush_size        = 1;
    data.brush_rotation    = 0;
    data.brush_opacity     = 1;
    data.height_blend_mode = HeightBlendMode::Alpha;
    data.modify_heights    = false;
    data.modify_albedo     = false;
    data.modify_mask       = true;       // <-- target real mask channel
    data.mask_index        = mask_index; // <-- which mask slot (0, 1, 2 ...)

    const int mask_flag = getLandscapeMaskFlag(mask_index);
    if (mask_flag == 0)
    {
        Log::warning("[TerrainManipulator] Invalid mask slot index %d\n", mask_index);
        return false;
    }

    int operation_id = Landscape::generateOperationID();
    m_pending_operations.emplace(operation_id, data);
    Landscape::asyncTextureDraw(operation_id, lmap->getGUID(), ivec2{ 0, 0 }, tile_res, mask_flag);

    Log::message("[TerrainManipulator] Mask painting queued for landscape mask slot %d\n", mask_index);
    return true;
}

/* --- CalculateDrawingRegion ---
 * Given a brush position and size in local space, compute the pixel coordinates
 * and size of the region to be modified on the LandscapeLayerMap. */
void TerrainManipulator::CalculateDrawingRegion(
    dvec3 brush_local_position, quat brush_local_rotation, dvec2 half_size,
    LandscapeLayerMapPtr closest_lmap,
    ivec2& drawing_region_coord, ivec2& drawing_region_size)
{
    Vec3 corners[4] = {
        brush_local_position + brush_local_rotation * Vec3(-half_size.x, -half_size.y, 0.0),
        brush_local_position + brush_local_rotation * Vec3( half_size.x, -half_size.y, 0.0),
        brush_local_position + brush_local_rotation * Vec3(-half_size.x,  half_size.y, 0.0),
        brush_local_position + brush_local_rotation * Vec3( half_size.x,  half_size.y, 0.0)
    };

    auto bbox_min = Vec2{
        min(min(corners[0].x, corners[1].x), min(corners[2].x, corners[3].x)),
        min(min(corners[0].y, corners[1].y), min(corners[2].y, corners[3].y))
    };
    auto bbox_max = Vec2{
        max(max(corners[0].x, corners[1].x), max(corners[2].x, corners[3].x)),
        max(max(corners[0].y, corners[1].y), max(corners[2].y, corners[3].y))
    };

    auto pixels_per_unit = Vec2{ closest_lmap->getResolution() } / Vec2{ closest_lmap->getSize() };

    drawing_region_coord = ivec2{ round(pixels_per_unit * bbox_min) };
    drawing_region_size = ivec2{ pixels_per_unit * round(bbox_max - bbox_min) };
}

/* --- PerformTerrainManipulationOperation ---
 * Generate an operation ID and request an async texture draw.
 * flags controls which data channels Unigine populates in the callback:
 *   FLAGS_DATA_HEIGHT | FLAGS_DATA_ALBEDO  -> height + albedo (default)
 *   FLAGS_DATA_MASK                       -> landscape mask channels */
void TerrainManipulator::PerformTerrainManipulationOperation(
    LandscapeLayerMapPtr lmap, ivec2 pixel_coord, ivec2 resolution, int flags)
{
    int id = Landscape::generateOperationID();
    if (m_pending_operations.find(id) == m_pending_operations.end())
    {
        // The caller must have inserted the pending operation before dispatch.
    }
    Landscape::asyncTextureDraw(id, lmap->getGUID(), pixel_coord, resolution, flags);
}

/* --- OnTextureDraw ---
 * Callback invoked by Landscape when an async texture draw operation completes.
 * Dequeues the next brush operation and applies it. */
void TerrainManipulator::OnTextureDraw(
    UGUID guid, int id, LandscapeTexturesPtr buffer, ivec2 coord, int data_mask)
{
    if (NumberOfRemainingOperations() == 0)
    {
        Log::message("No data to process, exiting terrain sculpt\n");
        m_in_progress = false;
        return;
    }

    m_in_progress = true;

    if (m_operations_blocked)
        return;

    auto operation_it = m_pending_operations.find(id);
    if (operation_it == m_pending_operations.end())
    {
        return;
    }

    BrushOperationData operation = operation_it->second;
    m_pending_operations.erase(operation_it);

    if (operation.height_image && operation.alpha_image)
    {
        auto height_texture = Texture::create();
        auto alpha_texture = Texture::create();
        if (!height_texture || !alpha_texture ||
            !height_texture->create(operation.height_image) ||
            !alpha_texture->create(operation.alpha_image))
        {
            Log::warning("[TerrainManipulator] Failed to create overwrite textures for terrain height update.\n");
        }
        else
        {
            Log::message("[TerrainManipulator] Applying direct overwrite height image (%d x %d)\n",
                         operation.height_image->getWidth(), operation.height_image->getHeight());
            SetTerrainHeightImmediate(nullptr, guid, id, buffer, coord, data_mask,
                                      height_texture, alpha_texture, true);
        }
    }
    else if (operation.albedo_image)
    {
        SetTerrainAlbedoImmediate(nullptr, guid, id, buffer, coord, data_mask, operation.albedo_image);
    }
    else
    {
        ApplyBrush(operation, guid, id, buffer, coord, data_mask);
    }

    if (NumberOfRemainingOperations() == 0)
    {
        Log::message("Finished Terrain Sculpting\n");
        m_in_progress = false;
    }
}

/* --- ApplyBrush ---
 * Configure the brush material with terrain data textures and parameters,
 * then execute the brush expression to modify the terrain.
 *
 * Unigine 2.18 mask path:
 *   - operation.modify_mask == true
 *   - buffer->getMask(mask_index) returns the TexturePtr for that slot
 *   - bind it to "terrain_mask" in the brush material
 *   - runExpression writes the result back into that mask channel */
void TerrainManipulator::ApplyBrush(
    BrushOperationData const& operation, UGUID guid, int id,
    LandscapeTexturesPtr buffer, ivec2 coord, int data_mask)
{
    auto brush_material = operation.brush_material;

    // ----------------------------------------------------------------
    // Real landscape mask channel (Unigine 2.18)
    // ----------------------------------------------------------------
    if (operation.modify_mask)
    {
        auto mask_texture = buffer->getMask(operation.mask_index);
        if (!mask_texture)
        {
            Log::warning("[TerrainManipulator] getMask(%d) returned null. "
                         "Ensure the LandscapeLayerMap has mask slot %d configured.\n",
                         operation.mask_index, operation.mask_index);
            return;
        }

        // Check if the brush material has a terrain_mask texture slot
        auto mask_texture_index = brush_material->findTexture("terrain_mask");
        if (mask_texture_index < 0)
        {
            Log::warning("[TerrainManipulator] Brush material has no 'terrain_mask' texture slot. "
                         "Mask painting requires a brush material that supports mask operations.\n");
            return;
        }

        // Bind the mask texture to the brush shader's "terrain_mask" slot
        brush_material->setTexture(mask_texture_index, mask_texture);
        brush_material->setParameterFloat("size",    operation.brush_size);
        brush_material->setParameterFloat("angle",   operation.brush_rotation);
        brush_material->setParameterFloat("opacity", operation.brush_opacity);
        brush_material->setParameterInt("data_mask", data_mask);
        brush_material->setState("height_blend_mode",
                                 static_cast<int>(operation.height_blend_mode));

        // Execute brush shader — writes result back into the mask texture
        brush_material->runExpression(
            "brush",
            buffer->getResolution().x,
            buffer->getResolution().y);

        // Unbind to avoid stale texture references
        brush_material->setTexture(mask_texture_index, nullptr);
        return;
    }

    // ----------------------------------------------------------------
    // Albedo channel
    // ----------------------------------------------------------------
    if (operation.modify_albedo)
        brush_material->setTexture("terrain_albedo", buffer->getAlbedo());

    // ----------------------------------------------------------------
    // Height channel
    // ----------------------------------------------------------------
    if (operation.modify_heights)
    {
        brush_material->setTexture("terrain_height", buffer->getHeight());
        brush_material->setTexture("terrain_opacity_height", buffer->getOpacityHeight());
        brush_material->setParameterFloat("height", operation.brush_height);
    }

    brush_material->setParameterFloat("size",    operation.brush_size);
    brush_material->setParameterFloat("angle",   operation.brush_rotation);
    brush_material->setParameterFloat("opacity", operation.brush_opacity);
    brush_material->setParameterInt("data_mask", data_mask);
    brush_material->setState("height_blend_mode",
                             static_cast<int>(operation.height_blend_mode));

    brush_material->runExpression("brush", buffer->getResolution().x, buffer->getResolution().y);

    // Unbind textures
    brush_material->setTexture("terrain_height", nullptr);
    brush_material->setTexture("terrain_opacity_height", nullptr);
    brush_material->setTexture("terrain_albedo", nullptr);
}

/* --- getCircularTerrainBrushWithFalloff ---
 * Create a circular brush material with falloff for terrain height operations. */
MaterialPtr TerrainManipulator::getCircularTerrainBrushWithFalloff(
    double falloff, double padding, std::string DEFAULT)
{
    auto file_guid = FileSystem::getGUID(FileSystem::resolvePartialVirtualPath(DEFAULT.c_str()));
    if (!file_guid.isValid())
    {
        Log::warning("Cannot find brush material: %s\n", DEFAULT.c_str());
        return nullptr;
    }

    auto brush_material = Materials::findMaterialByFileGUID(file_guid)->inherit();

    ImagePtr image = Image::create();
    int width = 512;
    int height = 512;
    image->create2D(width, height, Image::FORMAT_RGBA8);

    float FALLOFF_AS_RATIO = Math::clamp((float)falloff, 0.0f, 1.0f);
    int centerX = width / 2;
    int centerY = height / 2;
    float radius = (width - (float)padding * width) / 2.0f;

    Image::Pixel pix;
    pix.i.r = 255; pix.i.g = 255; pix.i.b = 255; pix.i.a = 255;

    auto calcPct = [](float x1, float x2, float y1) -> float
    {
        return (y1 - x1) / (x2 - x1);
    };

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            float dist = sqrtf((float)((x - centerX) * (x - centerX) + (y - centerY) * (y - centerY)));

            if (dist > radius)
            {
                pix.i.a = 0;
                image->set2D(x, y, pix);
                continue;
            }
            float ratio = calcPct(0, radius, dist);
            if (ratio < FALLOFF_AS_RATIO)
            {
                pix.i.a = 255;
                image->set2D(x, y, pix);
                continue;
            }
            float fd = radius * FALLOFF_AS_RATIO;
            float fr = calcPct(fd, radius, dist);
            float inv = 1.0f - fr;
            int alpha_int = Math::clamp(static_cast<int>(255.0f * inv), 0, 255);
            pix.i.a = alpha_int;
            image->set2D(x, y, pix);
        }
    }

    UGUID gui = UGUID();
    gui.generate();

    std::string temp_folder = std::string(FileSystem::getRootMount()->getDataPath()) + "/world_builder/temp/";
    std::stringstream name_ss;
    name_ss << temp_folder << gui.getString() << ".png";
    image->save(name_ss.str().c_str());
    brush_material->setTexturePath("opacity", name_ss.str().c_str());

    return brush_material;
}

/* --- getTerrainTileBrush ---
 * Create a brush material from a height/mask image. */
MaterialPtr TerrainManipulator::getTerrainTileBrush(ImagePtr height_image)
{
    if (!height_image)
        return nullptr;

    static const std::vector<std::string> brush_candidates = {
        "brush_without_contrast.basebrush",
        "circle_medium.brush",
        "circle_soft.brush",
        "circle_hard.brush"
    };

    UGUID file_guid;
    std::string resolved_brush_name;
    for (const auto& brush_name : brush_candidates)
    {
        auto guid = FileSystem::getGUID(FileSystem::resolvePartialVirtualPath(brush_name.c_str()));
        if (guid.isValid())
        {
            file_guid = guid;
            resolved_brush_name = brush_name;
            break;
        }
    }

    if (!file_guid.isValid())
    {
        static bool warned_missing_once = false;
        if (!warned_missing_once)
        {
            Log::warning("Could not find any tile brush material. Tried: "
                         "brush_without_contrast.basebrush, circle_medium.brush, "
                         "circle_soft.brush, circle_hard.brush\n");
            warned_missing_once = true;
        }
        return nullptr;
    }

    auto brush_material = Materials::findMaterialByFileGUID(file_guid)->inherit();
    auto texture_index = brush_material->findTexture("opacity");
    if (texture_index < 0)
    {
        Log::warning("Brush material '%s' has no 'opacity' texture slot\n",
                     resolved_brush_name.c_str());
        return nullptr;
    }

    brush_material->setTextureImage(texture_index, height_image);
    return brush_material;
}

/* --- getMaskTerrainBrush ---
 * Create a brush material specifically for mask painting with terrain_mask texture slot. */
MaterialPtr TerrainManipulator::getMaskTerrainBrush(ImagePtr mask_image)
{
    if (!mask_image)
        return nullptr;

    const char* mask_brush_name = "terrain_brush_r32f_overwrite.basebrush";
    auto file_guid = FileSystem::getGUID(FileSystem::resolvePartialVirtualPath(mask_brush_name));

    if (!file_guid.isValid())
    {
        Log::warning("Could not find mask brush material: %s\n", mask_brush_name);
        return nullptr;
    }

    auto brush_material = Materials::findMaterialByFileGUID(file_guid)->inherit();

    auto new_mask_texture_index = brush_material->findTexture("new_mask");
    if (new_mask_texture_index < 0)
    {
        Log::warning("Mask brush material '%s' has no 'new_mask' texture slot\n",
                     mask_brush_name);
        return nullptr;
    }

    brush_material->setTextureImage(new_mask_texture_index, mask_image);
    return brush_material;
}

/* ========================================================================
 * TerrainSurfaceToolEditorPlugin implementation (GantryLabelTool pattern)
 * ======================================================================== */

namespace UnigineEditor
{

TerrainSurfaceToolEditorPlugin::TerrainSurfaceToolEditorPlugin()
    : ui_panel_(nullptr), terrain_manipulator_(nullptr)
{
    Log::message("[TerrainSurfaceTool] Constructor called!\n");
}

TerrainSurfaceToolEditorPlugin::~TerrainSurfaceToolEditorPlugin() = default;

bool TerrainSurfaceToolEditorPlugin::init()
{
    Log::message("[TerrainSurfaceTool] INIT() called!\n");

    try
    {
        terrain_manipulator_ = std::make_unique<TerrainManipulator>();
        ui_panel_ = std::make_unique<UiPanelTerrain>(this);

        Log::message("[TerrainSurfaceTool] UI panel created\n");

        setupMenu();

        Log::message("[TerrainSurfaceTool] Menu setup complete\n");

        WindowManager::add(ui_panel_.get(), WindowManager::AreaType::ROOT_AREA_RIGHT);

        Log::message("[TerrainSurfaceTool] Plugin initialized successfully\n");
        return true;
    }
    catch (const std::exception& e)
    {
        Log::error("[TerrainSurfaceTool] Failed to initialize: %s\n", e.what());
        return false;
    }
}

void TerrainSurfaceToolEditorPlugin::shutdown()
{
    Log::message("[TerrainSurfaceTool] Plugin shutting down...\n");

    if (vamps_menu_ && terrain_tool_action_)
        vamps_menu_->removeAction(terrain_tool_action_);

    if (ui_panel_)
        WindowManager::remove(ui_panel_.get());

    ui_panel_.reset();
    terrain_manipulator_.reset();

    Log::message("[TerrainSurfaceTool] Plugin shutdown complete\n");
}

std::vector<NodePtr> TerrainSurfaceToolEditorPlugin::getSelectedMeshNodes()
{
    std::vector<NodePtr> result;
    const SelectorNodes* selector = Selection::getSelectorNodes();
    if (!selector) return result;

    const Vector<NodePtr>& nodes = selector->getNodes();
    for (const auto& node : nodes)
    {
        if (!node) continue;
        if (node->getType() == Node::OBJECT_MESH_STATIC ||
            node->getType() == Node::OBJECT_MESH_DYNAMIC)
            result.push_back(node);
        for (int i = 0; i < node->getNumChildren(); i++)
        {
            auto child = node->getChild(i);
            if (child && (child->getType() == Node::OBJECT_MESH_STATIC ||
                          child->getType() == Node::OBJECT_MESH_DYNAMIC))
                result.push_back(child);
        }
    }
    return result;
}

void TerrainSurfaceToolEditorPlugin::setupMenu()
{
    vamps_menu_ = WindowManager::findMenu("VampsPlugin");
    if (!vamps_menu_)
    {
        vamps_menu_ = new QMenu("VampsPlugin");
        Log::message("[TerrainSurfaceTool] Created VampsPlugin menu\n");
    }

    terrain_tool_action_ = new QAction("Terrain Surface Tool", this);
    connect(terrain_tool_action_, &QAction::triggered,
            this, &TerrainSurfaceToolEditorPlugin::openTerrainTool);

    if (vamps_menu_)
        vamps_menu_->addAction(terrain_tool_action_);
}

void TerrainSurfaceToolEditorPlugin::openTerrainTool()
{
    if (ui_panel_)
    {
        WindowManager::show(ui_panel_.get());
        WindowManager::activate(ui_panel_.get());
    }
}

} // namespace UnigineEditor

/* ========================================================================
 * UiPanelTerrain implementation
 * ======================================================================== */

UiPanelTerrain::UiPanelTerrain(UnigineEditor::TerrainSurfaceToolEditorPlugin* plugin)
    : QWidget()
    , plugin_(plugin)
{
    setupUI();
    updateSelectionStatus();

    setWindowTitle("Terrain Surface Tool");
    resize(400, 700);

    // Apply dark theme
    QPalette dark_palette;
    dark_palette.setColor(QPalette::Window, QColor(45, 45, 45));
    dark_palette.setColor(QPalette::WindowText, Qt::white);
    dark_palette.setColor(QPalette::Base, QColor(35, 35, 35));
    dark_palette.setColor(QPalette::AlternateBase, QColor(45, 45, 45));
    dark_palette.setColor(QPalette::ToolTipBase, Qt::white);
    dark_palette.setColor(QPalette::ToolTipText, Qt::white);
    dark_palette.setColor(QPalette::Text, Qt::white);
    dark_palette.setColor(QPalette::Button, QColor(60, 60, 60));
    dark_palette.setColor(QPalette::ButtonText, Qt::white);
    dark_palette.setColor(QPalette::BrightText, Qt::red);
    dark_palette.setColor(QPalette::Link, QColor(42, 130, 218));
    dark_palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
    dark_palette.setColor(QPalette::HighlightedText, Qt::black);
    setPalette(dark_palette);
}

UiPanelTerrain::~UiPanelTerrain() = default;

void UiPanelTerrain::setupUI()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setSpacing(10);
    main_layout->setContentsMargins(15, 15, 15, 15);

    // --- Header ---
    auto* header = new QLabel("Terrain Surface Tool", this);
    QFont header_font = header->font();
    header_font.setBold(true);
    header_font.setPointSize(14);
    header->setFont(header_font);
    main_layout->addWidget(header);

    // --- Selection Status ---
    selection_status_label_ = new QLabel("No mesh nodes selected", this);
    selection_status_label_->setStyleSheet("color: #ffcc00;");
    main_layout->addWidget(selection_status_label_);

    // --- Surface Filter ---
    auto* surface_group = new QGroupBox("Surface Filter", this);
    auto* surface_layout = new QHBoxLayout(surface_group);
    surface_layout->addWidget(new QLabel("Name/Regex:"));
    edit_surface_name_ = new QLineEdit("base", this);
    edit_surface_name_->setToolTip("Surface name or regex pattern to match on selected meshes");
    surface_layout->addWidget(edit_surface_name_);
    main_layout->addWidget(surface_group);

    // --- Mask Filter ---
    // mask_index is parsed from "mask_N" and passed directly to SetTerrainMask
    // so it targets the real Unigine landscape mask slot, not albedo
    auto* mask_group = new QGroupBox("Landscape Mask Slot", this);
    auto* mask_layout = new QHBoxLayout(mask_group);
    mask_layout->addWidget(new QLabel("Mask Name:"));
    edit_mask_name_ = new QLineEdit("mask_0", this);
    edit_mask_name_->setToolTip(
        "Landscape mask slot to paint. Format: mask_N (e.g. mask_0, mask_1).\n"
        "This targets the real Unigine landscape mask channel, not albedo.");
    mask_layout->addWidget(edit_mask_name_);
    main_layout->addWidget(mask_group);

    // --- Brush Settings ---
    auto* brush_group = new QGroupBox("Brush Settings", this);
    auto* brush_layout = new QVBoxLayout(brush_group);

    auto addSpinRow = [&](const QString& label, double min_val, double max_val,
                          double default_val, double step, const QString& tip) -> QDoubleSpinBox*
    {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(label));
        auto* spin = new QDoubleSpinBox(this);
        spin->setRange(min_val, max_val);
        spin->setValue(default_val);
        spin->setSingleStep(step);
        spin->setToolTip(tip);
        row->addWidget(spin);
        brush_layout->addLayout(row);
        return spin;
    };

    spin_brush_size_ = addSpinRow("Brush Size:", 0.1, 1000.0, 10.0, 1.0,
        "Size of the terrain brush in world units");
    spin_flat_distance_ = addSpinRow("Flat Distance:", 0.0, 500.0, 30.0, 5.0,
        "Distance from track to flatten terrain (pixels)");
    spin_falloff_distance_ = addSpinRow("Falloff Distance:", 0.0, 2000.0, 30.0, 5.0,
        "Distance over which terrain blends back (pixels)");
    spin_smooth_iterations_ = addSpinRow("Smooth Passes:", 1.0, 50.0, 3.0, 1.0,
        "Number of smoothing passes");
    spin_smooth_strength_ = addSpinRow("Smooth Strength:", 0.0, 1.0, 0.5, 0.1,
        "Strength of each smoothing pass (0=none, 1=max)");

    main_layout->addWidget(brush_group);

    // --- Action Buttons ---
    auto* actions_group = new QGroupBox("Actions", this);
    auto* actions_layout = new QVBoxLayout(actions_group);

    btn_apply_ = new QPushButton("Apply Pull Terrain", this);
    btn_apply_->setStyleSheet(
        "QPushButton { background-color: #2a7f2a; color: white; padding: 10px; font-weight: bold; }");
    connect(btn_apply_, &QPushButton::clicked, this, &UiPanelTerrain::onApplyPullTerrain);
    actions_layout->addWidget(btn_apply_);

    btn_apply_mask_ = new QPushButton("Apply to Landscape Mask", this);
    btn_apply_mask_->setStyleSheet(
        "QPushButton { background-color: #7f2a7f; color: white; padding: 8px; }");
    btn_apply_mask_->setToolTip(
        "Paint the selected mesh surface footprint into the landscape mask slot\n"
        "specified by 'Mask Name' above. Uses Unigine 2.18 getMask(index) API.");
    connect(btn_apply_mask_, &QPushButton::clicked, this, &UiPanelTerrain::onApplyToMask);
    actions_layout->addWidget(btn_apply_mask_);

    btn_smooth_ = new QPushButton("Smooth Terrain Around Selection", this);
    btn_smooth_->setStyleSheet(
        "QPushButton { background-color: #2a6f8f; color: white; padding: 8px; }");
    connect(btn_smooth_, &QPushButton::clicked, this, &UiPanelTerrain::onSmoothTerrain);
    actions_layout->addWidget(btn_smooth_);

    btn_reset_ = new QPushButton("Reset / Clear Heights", this);
    btn_reset_->setStyleSheet(
        "QPushButton { background-color: #8f2a2a; color: white; padding: 8px; }");
    connect(btn_reset_, &QPushButton::clicked, this, &UiPanelTerrain::onResetTerrainHeight);
    actions_layout->addWidget(btn_reset_);

    main_layout->addWidget(actions_group);

    // --- Progress ---
    progress_bar_ = new QProgressBar(this);
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    main_layout->addWidget(progress_bar_);

    // --- Status / Log ---
    main_layout->addWidget(new QLabel("Status:"));
    status_text_ = new QTextEdit(this);
    status_text_->setMaximumHeight(150);
    status_text_->setReadOnly(true);
    status_text_->setPlainText("Ready. Select mesh nodes in editor and click Apply.");
    main_layout->addWidget(status_text_);

    main_layout->addStretch();
}

void UiPanelTerrain::updateSelectionStatus()
{
    if (!plugin_) return;
    auto nodes = plugin_->getSelectedMeshNodes();
    if (nodes.empty())
    {
        selection_status_label_->setText("No mesh nodes selected");
        selection_status_label_->setStyleSheet("color: #ff6666;");
    }
    else
    {
        selection_status_label_->setText(
            QString("%1 mesh node(s) selected").arg(nodes.size()));
        selection_status_label_->setStyleSheet("color: #66ff66;");
    }
}

void UiPanelTerrain::logMessage(const QString& msg)
{
    if (status_text_)
    {
        status_text_->append(msg);
        status_text_->verticalScrollBar()->setValue(
            status_text_->verticalScrollBar()->maximum());
    }
    Unigine::Log::message("%s\n", msg.toUtf8().constData());
}

/* --- Apply Pull Terrain --- */
void UiPanelTerrain::onApplyPullTerrain()
{
    using namespace Unigine;
    using namespace Unigine::Math;

    logMessage("=== Pull Terrain To Surface ===");
    progress_bar_->setValue(0);

    auto selected = plugin_->getSelectedMeshNodes();
    if (selected.empty())
    {
        logMessage("ERROR: No mesh nodes selected.");
        return;
    }

    logMessage(QString("Found %1 mesh node(s)").arg(selected.size()));

    std::string surface_name = edit_surface_name_->text().toStdString();
    if (surface_name.empty())
    {
        logMessage("ERROR: Surface name is empty.");
        return;
    }

    double brush_size_val = spin_brush_size_->value();
    double flat_distance  = spin_flat_distance_->value();
    double falloff_dist   = spin_falloff_distance_->value();
    const double brush_radius = std::max(brush_size_val * 0.5, flat_distance + falloff_dist);
    const double brush_diameter = std::max(brush_size_val, brush_radius * 2.0);
    const double flat_ratio = brush_radius > 0.0
        ? std::clamp(flat_distance / brush_radius, 0.0, 1.0)
        : 1.0;

    logMessage(QString("Surface: '%1', Brush: %2, Flat: %3, Falloff: %4")
        .arg(QString::fromStdString(surface_name))
        .arg(brush_size_val).arg(flat_distance).arg(falloff_dist));

    ObjectLandscapeTerrainPtr terrain = Landscape::getActiveTerrain();
    if (!terrain)
    {
        logMessage("ERROR: No active landscape terrain.");
        return;
    }

    MaterialPtr pull_brush_material = TerrainManipulator::getCircularTerrainBrushWithFalloff(
        flat_ratio);
    if (!pull_brush_material)
        logMessage("WARNING: Brush material unavailable, falling back to tile-based terrain fill.");

    progress_bar_->setValue(10);
    int total = (int)selected.size();
    int processed = 0;

    for (auto& node : selected)
    {
        std::string node_name = node->getName();
        logMessage(QString("Processing: %1").arg(QString::fromStdString(node_name)));

        if (node->getType() == Node::OBJECT_MESH_STATIC)
        {
            auto oms = static_ptr_cast<ObjectMeshStatic>(node);
            const double smoothing_strength = spin_smooth_strength_->value();

            int num_surfaces = oms->getNumSurfaces();
            logMessage(QString("  Surfaces (%1):").arg(num_surfaces));
            for (int si = 0; si < num_surfaces; si++)
                logMessage(QString("    [%1] '%2'").arg(si).arg(oms->getSurfaceName(si)));

            auto processTileForPull = [&](LandscapeLayerMapPtr lmap, ObjectSurface obj_surface) -> bool
            {
                auto tile_res = lmap->getResolution();
                ImagePtr heights_image = Image::create();
                heights_image->create2D(tile_res.x, tile_res.y, Image::FORMAT_RGBA32F);

                Image::Pixel zero_pixel;
                zero_pixel.f.r = 0.0f;
                zero_pixel.f.g = 0.0f;
                zero_pixel.f.b = 0.0f;
                zero_pixel.f.a = 0.0f;
                for (int x = 0; x < tile_res.x; ++x)
                {
                    for (int y = 0; y < tile_res.y; ++y)
                        heights_image->set2D(x, y, zero_pixel);
                }

                std::map<int, int> height_data;
                if (!GetSurfaceHeightsForTerrainHeightmap(lmap, obj_surface, heights_image, height_data))
                    return false;

                logMessage(QString("  Modified %1 points").arg(height_data.size()));
                smoothTerrainHeights(lmap, heights_image, height_data, flat_distance, falloff_dist);

                if (smoothing_strength > 0.0)
                    smoothTerrainHeights(lmap, heights_image, height_data,
                                         flat_distance * smoothing_strength,
                                         falloff_dist * smoothing_strength);

                if (plugin_->getManipulator()->SetTerrainHeight(lmap, heights_image))
                {
                    logMessage("  Tile updated.");
                    return true;
                }

                logMessage("  WARNING: Failed to apply tile update.");
                return false;
            };

            int surface_id = oms->findSurface(surface_name.c_str());

            if (surface_id >= 0)
            {
                logMessage(QString("  Found surface '%1' (id=%2)")
                    .arg(QString::fromStdString(surface_name)).arg(surface_id));
                if (pull_brush_material)
                {
                    logMessage(QString("  Using brush-based terrain leveling (diameter %1, flat ratio %2)...")
                                   .arg(brush_diameter)
                                   .arg(flat_ratio, 0, 'f', 2));
                    plugin_->getManipulator()->RaiseTerrainToSurface(
                        node, surface_name, dvec2(brush_diameter, brush_diameter), pull_brush_material);
                }
                else
                {
                    logMessage("  Using tile-based terrain modification with smoothing...");

                    ObjectSurface obj_surface = std::make_pair(
                        static_ptr_cast<Object>(oms), surface_id);

                    for (int t = 0; t < terrain->getNumChildren(); t++)
                    {
                        auto lmap = checked_ptr_cast<LandscapeLayerMap>(terrain->getChild(t));
                        if (!lmap) continue;
                        if (!lmap->getWorldBoundBox().insideValid(oms->getWorldBoundBox()))
                            continue;

                        logMessage(QString("  Terrain tile %1...").arg(t));
                        processTileForPull(lmap, obj_surface);
                    }
                }
            }
            else
            {
                logMessage(QString("  '%1' not found, trying regex...")
                    .arg(QString::fromStdString(surface_name)));
                try
                {
                    std::regex rx(surface_name);
                    bool matched = false;
                    for (int s = 0; s < oms->getNumSurfaces(); s++)
                    {
                        std::string sn = oms->getSurfaceName(s);
                        if (std::regex_search(sn, rx))
                        {
                            matched = true;
                            logMessage(QString("  Regex matched: '%1' (id=%2)")
                                .arg(QString::fromStdString(sn)).arg(s));
                        }
                    }
                    if (matched)
                    {
                        if (pull_brush_material)
                        {
                            logMessage(QString("  Using brush-based terrain leveling (diameter %1, flat ratio %2)...")
                                           .arg(brush_diameter)
                                           .arg(flat_ratio, 0, 'f', 2));
                            plugin_->getManipulator()->RaiseTerrainToSurface(
                                node, surface_name, dvec2(brush_diameter, brush_diameter), pull_brush_material);
                        }
                        else
                        {
                            logMessage("  Using tile-based terrain modification with smoothing...");
                            for (int s = 0; s < oms->getNumSurfaces(); s++)
                            {
                                std::string sn = oms->getSurfaceName(s);
                                if (!std::regex_search(sn, rx))
                                    continue;

                                ObjectSurface obj_surface = std::make_pair(
                                    static_ptr_cast<Object>(oms), s);

                                for (int t = 0; t < terrain->getNumChildren(); t++)
                                {
                                    auto lmap = checked_ptr_cast<LandscapeLayerMap>(terrain->getChild(t));
                                    if (!lmap) continue;
                                    if (!lmap->getWorldBoundBox().insideValid(oms->getWorldBoundBox()))
                                        continue;

                                    logMessage(QString("  Terrain tile %1...").arg(t));
                                    processTileForPull(lmap, obj_surface);
                                }
                            }
                        }
                    }
                    else
                        logMessage("  WARNING: No surfaces matched. Check names above.");
                }
                catch (const std::regex_error& e)
                {
                    logMessage(QString("  Regex error: %1").arg(e.what()));
                }
            }
        }

        processed++;
        progress_bar_->setValue(10 + (processed * 80) / total);
    }

    progress_bar_->setValue(100);
    logMessage("=== Pull Terrain Complete ===");
}

/* ========================================================================
 * onApplyToMask  (Unigine 2.18 - Real Landscape Mask)
 *
 * Paints the footprint of the selected mesh surface into the landscape
 * mask slot specified by "Mask Name" (e.g. mask_0 = slot 0).
 *
 * Key differences from the old albedo approach:
 *   - mask_image is R8 grayscale (white = masked, black = no mask)
 *   - SetTerrainMask uses FLAGS_DATA_MASKS + getMask(index)
 *   - No RGBA colour manipulation; alpha channel not used
 * ======================================================================== */
void UiPanelTerrain::onApplyToMask()
{
    using namespace Unigine;
    using namespace Unigine::Math;

    logMessage("=== Apply to Landscape Mask ===");
    progress_bar_->setValue(0);

    auto selected = plugin_->getSelectedMeshNodes();
    if (selected.empty())
    {
        logMessage("ERROR: No mesh nodes selected.");
        return;
    }

    logMessage(QString("Found %1 mesh node(s)").arg(selected.size()));

    std::string surface_name = edit_surface_name_->text().toStdString();
    if (surface_name.empty())
    {
        logMessage("ERROR: Surface name is empty.");
        return;
    }

    std::string mask_name = edit_mask_name_->text().toStdString();
    if (mask_name.empty())
    {
        logMessage("ERROR: Mask name is empty.");
        return;
    }

    // Parse mask slot index from "mask_N"
    int mask_index = 0;
    if (mask_name.find("mask_") == 0)
    {
        try
        {
            mask_index = std::stoi(mask_name.substr(5));
            if (mask_index < 0)
            {
                logMessage(QString("ERROR: Invalid mask index %1 (must be >= 0)").arg(mask_index));
                return;
            }
        }
        catch (...)
        {
            logMessage(QString("ERROR: Invalid mask name format. Expected 'mask_N', got '%1'")
                .arg(QString::fromStdString(mask_name)));
            return;
        }
    }
    else
    {
        logMessage(QString("ERROR: Invalid mask name format. Expected 'mask_N', got '%1'")
            .arg(QString::fromStdString(mask_name)));
        return;
    }

    double brush_size_val = spin_brush_size_->value();
    double flat_distance  = spin_flat_distance_->value();
    double falloff_dist   = spin_falloff_distance_->value();

    logMessage(QString("Surface: '%1', Landscape Mask Slot: %2, Brush: %3, Flat: %4, Falloff: %5")
        .arg(QString::fromStdString(surface_name)).arg(mask_index)
        .arg(brush_size_val).arg(flat_distance).arg(falloff_dist));

    ObjectLandscapeTerrainPtr terrain = Landscape::getActiveTerrain();
    if (!terrain)
    {
        logMessage("ERROR: No active landscape terrain.");
        return;
    }

    logMessage(QString("Painting landscape mask slot %1 via getMask() API...").arg(mask_index));
    progress_bar_->setValue(10);
    int total = (int)selected.size();
    int processed = 0;

    for (auto& node : selected)
    {
        std::string node_name = node->getName();
        logMessage(QString("Processing: %1").arg(QString::fromStdString(node_name)));

        if (node->getType() == Node::OBJECT_MESH_STATIC)
        {
            auto oms = static_ptr_cast<ObjectMeshStatic>(node);

            int num_surfaces = oms->getNumSurfaces();
            logMessage(QString("  Surfaces (%1):").arg(num_surfaces));
            for (int si = 0; si < num_surfaces; si++)
                logMessage(QString("    [%1] '%2'").arg(si).arg(oms->getSurfaceName(si)));

            int surface_id = oms->findSurface(surface_name.c_str());

            // --------------------------------------------------------
            // Helper lambda: build R8 grayscale mask image for one tile
            // and submit it to SetTerrainMask (real landscape mask slot)
            // --------------------------------------------------------
            auto processTileForMask = [&](LandscapeLayerMapPtr lmap,
                                          ObjectSurface obj_surface) -> bool
            {
                auto tile_res = lmap->getResolution();

                // Use R8 grayscale - white = fully masked, black = no mask
                // (Unigine 2.18 landscape mask format)
                ImagePtr mask_image = Image::create();
                mask_image->create2D(tile_res.x, tile_res.y, Image::FORMAT_R8);

                // Initialize all pixels to 0 (no mask)
                Image::Pixel zp;
                zp.i.r = 0;
                for (int x = 0; x < tile_res.x; x++)
                    for (int y = 0; y < tile_res.y; y++)
                        mask_image->set2D(x, y, zp);

                // We need GetSurfaceHeightsForTerrain to accept R8 image.
                // If your implementation requires RGBA32F as input, create
                // a temporary RGBA32F image, then convert to R8 after filling.
                ImagePtr rgba_scratch = Image::create();
                rgba_scratch->create2D(tile_res.x, tile_res.y, Image::FORMAT_RGBA32F);

                Image::Pixel zp_rgba; zp_rgba.f.r=0; zp_rgba.f.g=0; zp_rgba.f.b=0; zp_rgba.f.a=0;
                for (int x = 0; x < tile_res.x; x++)
                    for (int y = 0; y < tile_res.y; y++)
                        rgba_scratch->set2D(x, y, zp_rgba);

                std::map<int,int> height_data;
                if (!GetSurfaceHeightsForTerrain(lmap, obj_surface, rgba_scratch, height_data))
                    return false;

                logMessage(QString("  Painted %1 mask points on tile").arg(height_data.size()));

                // Apply falloff to mask edges using the RGBA scratch image
                applyMaskFalloff(rgba_scratch, height_data, falloff_dist);

                // Transfer alpha channel from RGBA scratch -> R8 mask image.
                // Flip Y to match the terrain image coordinate convention used
                // by the helper routines that generated the scratch texture.
                for (int x = 0; x < tile_res.x; x++)
                {
                    for (int y = 0; y < tile_res.y; y++)
                    {
                        Image::Pixel src = rgba_scratch->get2D(x, y);
                        Image::Pixel dst;
                        // Convert alpha float [0..1] -> R8 uint [0..255]
                        dst.i.r = Math::clamp(static_cast<int>(src.f.a * 255.0f), 0, 255);
                        mask_image->set2D(x, tile_res.y - 1 - y, dst);
                    }
                }

                dvec2 brush_size_vec(brush_size_val, brush_size_val);

                // SetTerrainMask now targets the real landscape mask slot
                // using FLAGS_DATA_MASKS + getMask(mask_index)
                if (plugin_->getManipulator()->SetTerrainMask(
                        lmap, node, mask_image, brush_size_vec, mask_index))
                {
                    logMessage(QString("  Landscape mask slot %1 applied to tile")
                        .arg(mask_index));
                    return true;
                }
                else
                {
                    logMessage("  WARNING: Failed to apply landscape mask.");
                    return false;
                }
            };

            if (surface_id >= 0)
            {
                logMessage(QString("  Found surface '%1' (id=%2)")
                    .arg(QString::fromStdString(surface_name)).arg(surface_id));

                ObjectSurface obj_surface = std::make_pair(
                    static_ptr_cast<Object>(oms), surface_id);

                for (int t = 0; t < terrain->getNumChildren(); t++)
                {
                    auto lmap = checked_ptr_cast<LandscapeLayerMap>(terrain->getChild(t));
                    if (!lmap) continue;
                    if (!lmap->getWorldBoundBox().insideValid(oms->getWorldBoundBox()))
                        continue;

                    logMessage(QString("  Terrain tile %1...").arg(t));
                    processTileForMask(lmap, obj_surface);
                }
            }
            else
            {
                logMessage(QString("  '%1' not found, trying regex...")
                    .arg(QString::fromStdString(surface_name)));
                try
                {
                    std::regex rx(surface_name);
                    bool matched = false;
                    for (int s = 0; s < oms->getNumSurfaces(); s++)
                    {
                        std::string sn = oms->getSurfaceName(s);
                        if (std::regex_search(sn, rx))
                        {
                            matched = true;
                            logMessage(QString("  Regex matched: '%1' (id=%2)")
                                .arg(QString::fromStdString(sn)).arg(s));

                            ObjectSurface obj_surface = std::make_pair(
                                static_ptr_cast<Object>(oms), s);

                            for (int t = 0; t < terrain->getNumChildren(); t++)
                            {
                                auto lmap = checked_ptr_cast<LandscapeLayerMap>(
                                    terrain->getChild(t));
                                if (!lmap) continue;
                                if (!lmap->getWorldBoundBox().insideValid(oms->getWorldBoundBox()))
                                    continue;

                                processTileForMask(lmap, obj_surface);
                            }
                        }
                    }
                    if (!matched)
                        logMessage("  WARNING: No surfaces matched. Check names above.");
                }
                catch (const std::regex_error& e)
                {
                    logMessage(QString("  Regex error: %1").arg(e.what()));
                }
            }
        }
        else
        {
            logMessage(QString("  Skipping non-mesh node type: %1").arg(node->getType()));
        }

        processed++;
        progress_bar_->setValue(10 + (processed * 80) / total);
    }

    progress_bar_->setValue(100);
    logMessage("=== Apply to Landscape Mask Complete ===");
}

/* --- Smooth Terrain Around Selection --- */
void UiPanelTerrain::onSmoothTerrain()
{
    using namespace Unigine;
    using namespace Unigine::Math;

    logMessage("=== Smooth Terrain ===");
    progress_bar_->setValue(0);

    auto selected = plugin_->getSelectedMeshNodes();
    if (selected.empty())
    {
        logMessage("ERROR: No mesh nodes selected.");
        return;
    }

    ObjectLandscapeTerrainPtr terrain = Landscape::getActiveTerrain();
    if (!terrain)
    {
        logMessage("ERROR: No active landscape terrain.");
        return;
    }

    std::string surface_name = edit_surface_name_->text().toStdString();
    double flat_dist   = spin_flat_distance_->value();
    double falloff     = spin_falloff_distance_->value();
    int    passes      = (int)spin_smooth_iterations_->value();
    double strength    = spin_smooth_strength_->value();

    logMessage(QString("Passes: %1, Strength: %2").arg(passes).arg(strength));

    int total = (int)selected.size();
    int processed = 0;

    for (auto& node : selected)
    {
        if (node->getType() != Node::OBJECT_MESH_STATIC) continue;
        auto oms = static_ptr_cast<ObjectMeshStatic>(node);

        int sid = oms->findSurface(surface_name.c_str());
        if (sid < 0)
        {
            if (oms->getNumSurfaces() > 0) sid = 0;
            else continue;
        }

        ObjectSurface obj_surface = std::make_pair(static_ptr_cast<Object>(oms), sid);

        for (int t = 0; t < terrain->getNumChildren(); t++)
        {
            auto lmap = checked_ptr_cast<LandscapeLayerMap>(terrain->getChild(t));
            if (!lmap) continue;
            if (!lmap->getWorldBoundBox().insideValid(oms->getWorldBoundBox()))
                continue;

            auto tile_res = lmap->getResolution();
            ImagePtr heights_image = Image::create();
            heights_image->create2D(tile_res.x, tile_res.y, Image::FORMAT_RGBA32F);

            Image::Pixel zp; zp.f.r=0; zp.f.g=0; zp.f.b=0; zp.f.a=0;
            for (int x = 0; x < tile_res.x; x++)
                for (int y = 0; y < tile_res.y; y++)
                    heights_image->set2D(x, y, zp);

            std::map<int,int> height_data;
            if (GetSurfaceHeightsForTerrainHeightmap(lmap, obj_surface, heights_image, height_data))
            {
                for (int p = 0; p < passes; p++)
                    smoothTerrainHeights(lmap, heights_image, height_data,
                                         flat_dist * strength, falloff * strength);

                if (plugin_->getManipulator()->SetTerrainHeight(lmap, heights_image))
                    logMessage(QString("  Smoothed tile %1 (%2 passes)").arg(t).arg(passes));
                else
                    logMessage(QString("  WARNING: Failed to smooth tile %1 "
                                       "(brush material missing)").arg(t));
            }
        }

        processed++;
        progress_bar_->setValue((processed * 100) / total);
    }

    progress_bar_->setValue(100);
    logMessage("=== Smooth Complete ===");
}

/* --- Reset / Clear Terrain Heights --- */
void UiPanelTerrain::onResetTerrainHeight()
{
    using namespace Unigine;
    using namespace Unigine::Math;

    logMessage("=== Reset Terrain Heights ===");
    progress_bar_->setValue(0);

    auto selected = plugin_->getSelectedMeshNodes();
    if (selected.empty())
    {
        logMessage("ERROR: No mesh nodes selected.");
        return;
    }

    ObjectLandscapeTerrainPtr terrain = Landscape::getActiveTerrain();
    if (!terrain)
    {
        logMessage("ERROR: No active landscape terrain.");
        return;
    }

    int total = (int)selected.size();
    int processed = 0;

    for (auto& node : selected)
    {
        if (node->getType() != Node::OBJECT_MESH_STATIC) continue;
        auto oms = static_ptr_cast<ObjectMeshStatic>(node);

        for (int t = 0; t < terrain->getNumChildren(); t++)
        {
            auto lmap = checked_ptr_cast<LandscapeLayerMap>(terrain->getChild(t));
            if (!lmap) continue;
            if (!lmap->getWorldBoundBox().insideValid(oms->getWorldBoundBox()))
                continue;

            auto tile_res = lmap->getResolution();
            ImagePtr flat_image = Image::create();
            flat_image->create2D(tile_res.x, tile_res.y, Image::FORMAT_RGBA32F);

            Image::Pixel fp; fp.f.r = 0; fp.f.g = 0; fp.f.b = 0; fp.f.a = 1.0f;
            for (int x = 0; x < tile_res.x; x++)
                for (int y = 0; y < tile_res.y; y++)
                    flat_image->set2D(x, y, fp);

            if (plugin_->getManipulator()->SetTerrainHeight(lmap, flat_image))
                logMessage(QString("  Reset tile %1 to zero height").arg(t));
            else
                logMessage(QString("  WARNING: Failed to reset tile %1 "
                                   "(brush material missing)").arg(t));
        }

        processed++;
        progress_bar_->setValue((processed * 100) / total);
    }

    progress_bar_->setValue(100);
    logMessage("=== Reset Complete ===");
}
