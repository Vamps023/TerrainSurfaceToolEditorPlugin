#define _USE_MATH_DEFINES

#include "TerrainManipulator.h"

#include <UnigineFileSystem.h>
#include <UnigineLog.h>

#include <cmath>

using namespace Unigine;
using namespace Unigine::Math;

namespace
{
double clampPositive(double value)
{
    return std::max(0.0, value);
}

float safeFlatRatio(double flat_distance, double falloff_distance, double brush_size)
{
    const double radius = std::max(brush_size * 0.5, flat_distance + falloff_distance);
    if (radius <= Consts::EPS_D)
        return 1.0f;
    return static_cast<float>(clamp(flat_distance / radius, 0.0, 1.0));
}

float sampleSpacingForBrush(double brush_size)
{
    return static_cast<float>(clamp(brush_size * 0.2, 1.0, 10.0));
}

double effectiveFlatDistance(const TerrainBrushSettings& settings)
{
    return clampPositive(settings.flat_distance) + (clampPositive(settings.brush_size) * 0.5);
}
}

TerrainManipulator::TerrainManipulator(LandscapeSaveManager& save_manager)
    : save_manager_(save_manager)
{
    texture_draw_connection_id_ = Landscape::getEventTextureDraw().connect(
        [this](const UGUID& guid, int operation_id, const LandscapeTexturesPtr& buffer,
               const ivec2& coord, int data_mask)
        {
            onTextureDraw(guid, operation_id, buffer, coord, data_mask);
        });
}

TerrainManipulator::~TerrainManipulator()
{
    if (texture_draw_connection_id_)
    {
        Landscape::getEventTextureDraw().disconnect(texture_draw_connection_id_);
        texture_draw_connection_id_ = nullptr;
    }

    pending_operations_.clear();
    pending_transaction_commits_ = 0;
    in_progress_ = false;
}

bool TerrainManipulator::pullTerrainToSurface(const std::vector<NodePtr>& nodes,
                                              const ObjectLandscapeTerrainPtr& terrain,
                                              const LandscapeLayerMapPtr& target_tile,
                                              const std::string& surface_pattern,
                                              const TerrainBrushSettings& settings,
                                              const LogFn& log)
{
    if (nodes.empty())
        return false;

    SurfaceRasterizer::SurfaceQuery query;
    std::string query_error;
    if (!SurfaceRasterizer::buildSurfaceQuery(surface_pattern, query, query_error))
    {
        if (log)
            log(query_error);
        return false;
    }

    TerrainContext terrain_context = buildTerrainContext(terrain, target_tile);
    if (!terrain_context.terrain)
    {
        if (log)
            log("ERROR: No active landscape terrain.");
        return false;
    }

    beginActionTransaction();

    const float flat_ratio = safeFlatRatio(settings.flat_distance, settings.falloff_distance, settings.brush_size);
    const MaterialPtr pull_brush_material = createCircularBrush(flat_ratio);
    const bool use_brush_path = pull_brush_material != nullptr;
    const double falloff_flat_distance = effectiveFlatDistance(settings);

    bool queued_any_operation = false;

    for (const auto& node : nodes)
    {
        if (!node || node->getType() != Node::OBJECT_MESH_STATIC)
            continue;

        ObjectMeshStaticPtr mesh = checked_ptr_cast<ObjectMeshStatic>(node);
        if (!mesh)
            continue;

        const std::vector<int> surface_ids = SurfaceRasterizer::findMatchingSurfaceIds(mesh, query);
        if (surface_ids.empty())
            continue;

        for (int surface_id : surface_ids)
        {
            if (use_brush_path)
            {
                std::vector<vec3> surface_vertices;
                std::vector<vec3> midpoints;
                std::vector<vec3> samples;
                if (!SurfaceRasterizer::extractSurfaceVerticesWorldSpace(mesh, surface_id, surface_vertices))
                    continue;

                SurfaceRasterizer::buildMidpointPath(surface_vertices, midpoints);
                SurfaceRasterizer::samplePolyline(midpoints,
                                                 sampleSpacingForBrush(settings.brush_size),
                                                 samples);
                if (samples.empty())
                    continue;

                const double brush_radius = std::max(settings.brush_size * 0.5,
                                                     settings.flat_distance + settings.falloff_distance);
                const double brush_diameter = std::max(settings.brush_size, brush_radius * 2.0);
                const dvec2 brush_size_value(brush_diameter, brush_diameter);

                auto calculateDirection = [](const vec3& from, const vec3& to) -> float
                {
                    const double angle_radians = std::atan2(static_cast<double>(to.y - from.y),
                                                            static_cast<double>(to.x - from.x));
                    return static_cast<float>((angle_radians * 180.0 / M_PI) + 90.0);
                };

                float brush_rotation = 0.0f;
                for (size_t sample_index = 0; sample_index < samples.size(); ++sample_index)
                {
                    if (sample_index + 1 < samples.size())
                        brush_rotation = calculateDirection(samples[sample_index], samples[sample_index + 1]);

                    const vec3& sample = samples[sample_index];
                    raiseTerrainAtPoint(terrain_context,
                                        dvec3(sample.x, sample.y, sample.z),
                                        brush_size_value,
                                        pull_brush_material,
                                        brush_rotation,
                                        false,
                                        nullptr);
                    queued_any_operation = true;
                }
            }
            else
            {
                ObjectSurface object_surface = std::make_pair(static_ptr_cast<Object>(mesh), surface_id);
                const WorldBoundBox node_bounds = mesh->getWorldBoundBox();
                SurfaceRasterizer::RasterBuffer raster_buffer;

                for (const auto& tile : terrain_context.layer_maps)
                {
                    if (!tile || !tile->getWorldBoundBox().insideValid(node_bounds))
                        continue;

                    if (!SurfaceRasterizer::rasterizeSurfaceHeight(tile, object_surface, raster_buffer))
                        continue;

                    SurfaceRasterizer::applyDistanceFalloff(tile,
                                                            raster_buffer,
                                                            falloff_flat_distance,
                                                            settings.falloff_distance);
                    if (settings.smoothing_strength > 0.0)
                    {
                        SurfaceRasterizer::applyDistanceFalloff(tile,
                                                                raster_buffer,
                                                                falloff_flat_distance * settings.smoothing_strength,
                                                                settings.falloff_distance * settings.smoothing_strength);
                    }

                    const ImagePtr height_image = SurfaceRasterizer::createHeightImage(raster_buffer);
                    if (height_image && setTerrainHeight(tile, height_image))
                        queued_any_operation = true;
                }
            }
        }
    }

    finishActionScheduling();
    if (!queued_any_operation && log)
        log("WARNING: No terrain operations were queued.");
    return queued_any_operation;
}

bool TerrainManipulator::applyLandscapeMask(const std::vector<NodePtr>& nodes,
                                            const ObjectLandscapeTerrainPtr& terrain,
                                            const LandscapeLayerMapPtr& target_tile,
                                            const std::string& surface_pattern,
                                            const TerrainBrushSettings& settings,
                                            int mask_index,
                                            const LogFn& log)
{
    if (nodes.empty())
        return false;

    SurfaceRasterizer::SurfaceQuery query;
    std::string query_error;
    if (!SurfaceRasterizer::buildSurfaceQuery(surface_pattern, query, query_error))
    {
        if (log)
            log(query_error);
        return false;
    }

    TerrainContext terrain_context = buildTerrainContext(terrain, target_tile);
    if (!terrain_context.terrain)
    {
        if (log)
            log("ERROR: No active landscape terrain.");
        return false;
    }

    beginActionTransaction();
    bool queued_any_operation = false;
    const double mask_flat_distance = effectiveFlatDistance(settings);

    for (const auto& node : nodes)
    {
        if (!node || node->getType() != Node::OBJECT_MESH_STATIC)
            continue;

        ObjectMeshStaticPtr mesh = checked_ptr_cast<ObjectMeshStatic>(node);
        if (!mesh)
            continue;

        const std::vector<int> surface_ids = SurfaceRasterizer::findMatchingSurfaceIds(mesh, query);
        if (surface_ids.empty())
            continue;

        const WorldBoundBox node_bounds = mesh->getWorldBoundBox();
        for (int surface_id : surface_ids)
        {
            ObjectSurface object_surface = std::make_pair(static_ptr_cast<Object>(mesh), surface_id);
            SurfaceRasterizer::RasterBuffer raster_buffer;

            for (const auto& tile : terrain_context.layer_maps)
            {
                if (!tile || !tile->getWorldBoundBox().insideValid(node_bounds))
                    continue;

                if (!SurfaceRasterizer::rasterizeSurfaceMask(tile, object_surface, raster_buffer))
                    continue;

                SurfaceRasterizer::applyDistanceFalloff(tile,
                                                        raster_buffer,
                                                        mask_flat_distance,
                                                        settings.falloff_distance);

                const ImagePtr mask_image = SurfaceRasterizer::createMaskImage(raster_buffer);
                if (mask_image && setTerrainMask(tile, mask_image, settings, mask_index))
                    queued_any_operation = true;
            }
        }
    }

    finishActionScheduling();
    if (!queued_any_operation && log)
        log("WARNING: No landscape mask operations were queued.");
    return queued_any_operation;
}

bool TerrainManipulator::resetTerrainHeights(const std::vector<NodePtr>& nodes,
                                             const ObjectLandscapeTerrainPtr& terrain,
                                             const LandscapeLayerMapPtr& target_tile,
                                             const LogFn& log)
{
    if (nodes.empty())
        return false;

    TerrainContext terrain_context = buildTerrainContext(terrain, target_tile);
    if (!terrain_context.terrain)
    {
        if (log)
            log("ERROR: No active landscape terrain.");
        return false;
    }

    beginActionTransaction();
    bool queued_any_operation = false;

    for (const auto& node : nodes)
    {
        if (!node || node->getType() != Node::OBJECT_MESH_STATIC)
            continue;

        ObjectMeshStaticPtr mesh = checked_ptr_cast<ObjectMeshStatic>(node);
        if (!mesh)
            continue;

        for (const auto& tile : terrain_context.layer_maps)
        {
            if (!tile || !tile->getWorldBoundBox().insideValid(mesh->getWorldBoundBox()))
                continue;

            const ivec2 tile_resolution = tile->getResolution();
            ImagePtr height_image = Image::create();
            height_image->create2D(tile_resolution.x, tile_resolution.y, Image::FORMAT_RGBA32F);

            Image::Pixel pixel;
            pixel.f.r = 0.0f;
            pixel.f.g = 0.0f;
            pixel.f.b = 0.0f;
            pixel.f.a = 1.0f;

            for (int x = 0; x < tile_resolution.x; ++x)
            {
                for (int y = 0; y < tile_resolution.y; ++y)
                    height_image->set2D(x, y, pixel);
            }

            if (setTerrainHeight(tile, height_image))
                queued_any_operation = true;
        }
    }

    finishActionScheduling();
    if (!queued_any_operation && log)
        log("WARNING: No terrain tiles intersected the selected meshes.");
    return queued_any_operation;
}

bool TerrainManipulator::isBusy() const
{
    return in_progress_ || !pending_operations_.empty();
}

size_t TerrainManipulator::pendingOperationCount() const
{
    return pending_operations_.size();
}

void TerrainManipulator::flushPendingSaves()
{
    save_manager_.forceFlush();
}

bool TerrainManipulator::beginActionTransaction()
{
    save_manager_.beginTransaction();
    return true;
}

void TerrainManipulator::finishActionScheduling()
{
    if (pending_operations_.empty())
    {
        save_manager_.endTransaction();
        return;
    }

    ++pending_transaction_commits_;
    in_progress_ = true;
}

void TerrainManipulator::finalizeActionTransactionsIfIdle()
{
    if (!pending_operations_.empty())
        return;

    while (pending_transaction_commits_ > 0)
    {
        save_manager_.endTransaction();
        --pending_transaction_commits_;
    }

    in_progress_ = false;
}

TerrainManipulator::TerrainContext TerrainManipulator::buildTerrainContext(const ObjectLandscapeTerrainPtr& terrain,
                                                                          const LandscapeLayerMapPtr& target_tile)
{
    TerrainContext context;
    context.terrain = terrain ? terrain : Landscape::getActiveTerrain();
    context.fetch = LandscapeFetch::create();
    if (!context.terrain)
        return context;

    if (target_tile)
    {
        context.layer_maps.push_back(target_tile);
        return context;
    }

    context.layer_maps.reserve(context.terrain->getNumChildren());
    for (int child_index = 0; child_index < context.terrain->getNumChildren(); ++child_index)
    {
        auto tile = checked_ptr_cast<LandscapeLayerMap>(context.terrain->getChild(child_index));
        if (tile)
            context.layer_maps.push_back(tile);
    }

    return context;
}

bool TerrainManipulator::terrainAvailable(TerrainContext& context, const dvec2& position)
{
    if (!context.fetch)
        context.fetch = LandscapeFetch::create();
    return context.fetch && context.fetch->fetchForce(position);
}

LandscapeLayerMapPtr TerrainManipulator::findContainingLayerMap(const TerrainContext& context,
                                                                const dvec3& position)
{
    LandscapeLayerMapPtr best_match = nullptr;
    for (const auto& tile : context.layer_maps)
    {
        if (!tile || !tile->getWorldBoundBox().inside(position))
            continue;
        if (!best_match || tile->getOrder() > best_match->getOrder())
            best_match = tile;
    }
    return best_match;
}

void TerrainManipulator::raiseTerrainAtPoint(const TerrainContext& context,
                                             const dvec3& point,
                                             const dvec2& brush_size,
                                             const MaterialPtr& brush_material,
                                             float brush_rotation,
                                             bool target_albedo,
                                             LandscapeLayerMapPtr forced_tile)
{
    TerrainContext mutable_context = context;
    if (!terrainAvailable(mutable_context, dvec2(point.x, point.y)))
        return;

    LandscapeLayerMapPtr tile = forced_tile ? forced_tile : findContainingLayerMap(context, point);
    if (!tile)
        return;

    const dvec3 brush_local_position = tile->getIWorldTransform() * point;
    const quat brush_world_rotation = quat(vec3_up, brush_rotation);
    const quat brush_local_rotation = brush_world_rotation * inverse(tile->getWorldRotation());
    const dvec2 half_size = brush_size * 0.5;

    ivec2 drawing_coord;
    ivec2 drawing_size;
    calculateDrawingRegion(brush_local_position, brush_local_rotation, half_size, tile, drawing_coord, drawing_size);

    drawing_coord.x = clamp(drawing_coord.x, 0, tile->getResolution().x - 1);
    drawing_coord.y = clamp(drawing_coord.y, 0, tile->getResolution().y - 1);
    drawing_size.x = clamp(drawing_size.x, 1, tile->getResolution().x - drawing_coord.x);
    drawing_size.y = clamp(drawing_size.y, 1, tile->getResolution().y - drawing_coord.y);

    BrushOperationData operation;
    operation.brush_material = brush_material;
    operation.brush_height = static_cast<float>(point.z);
    operation.brush_size = static_cast<float>(std::max(drawing_size.x, drawing_size.y));
    operation.brush_rotation = brush_rotation;
    operation.modify_heights = !target_albedo;
    operation.modify_albedo = target_albedo;

    const int flags = target_albedo
        ? Landscape::FLAGS_FILE_DATA_ALBEDO
        : (Landscape::FLAGS_FILE_DATA_HEIGHT | Landscape::FLAGS_FILE_DATA_OPACITY_HEIGHT);

    const int operation_id = Landscape::generateOperationID();
    pending_operations_[operation_id] = operation;
    Landscape::asyncTextureDraw(operation_id, tile->getGUID(), drawing_coord, drawing_size, flags);

    if (forced_tile)
        return;

    const int max_x = tile->getResolution().x - drawing_size.x;
    const int max_y = tile->getResolution().y - drawing_size.y;
    if ((drawing_coord.x > 0 && drawing_coord.x < max_x) &&
        (drawing_coord.y > 0 && drawing_coord.y < max_y))
    {
        return;
    }

    dvec3 boundary_probe = point;
    const dvec3 tile_position = tile->getWorldPosition();
    boundary_probe.x = (drawing_coord.x <= 0)
        ? tile_position.x + drawing_coord.x
        : tile_position.x + drawing_coord.x + drawing_size.x;
    boundary_probe.y = (drawing_coord.y <= 0)
        ? tile_position.y + drawing_coord.y
        : tile_position.y + drawing_coord.y + drawing_size.y;

    LandscapeLayerMapPtr neighbor_tile = findContainingLayerMap(context, boundary_probe);
    if (!neighbor_tile || neighbor_tile == tile)
        return;

    raiseTerrainAtPoint(context, point, brush_size, brush_material, brush_rotation, target_albedo, neighbor_tile);
}

void TerrainManipulator::calculateDrawingRegion(const dvec3& brush_local_position,
                                                const quat& brush_local_rotation,
                                                const dvec2& half_size,
                                                const LandscapeLayerMapPtr& tile,
                                                ivec2& out_coord,
                                                ivec2& out_size)
{
    const Vec3 corners[4] = {
        brush_local_position + brush_local_rotation * Vec3(-half_size.x, -half_size.y, 0.0),
        brush_local_position + brush_local_rotation * Vec3( half_size.x, -half_size.y, 0.0),
        brush_local_position + brush_local_rotation * Vec3(-half_size.x,  half_size.y, 0.0),
        brush_local_position + brush_local_rotation * Vec3( half_size.x,  half_size.y, 0.0),
    };

    const Vec2 bbox_min(
        std::min(std::min(corners[0].x, corners[1].x), std::min(corners[2].x, corners[3].x)),
        std::min(std::min(corners[0].y, corners[1].y), std::min(corners[2].y, corners[3].y)));
    const Vec2 bbox_max(
        std::max(std::max(corners[0].x, corners[1].x), std::max(corners[2].x, corners[3].x)),
        std::max(std::max(corners[0].y, corners[1].y), std::max(corners[2].y, corners[3].y)));

    const Vec2 pixels_per_unit = Vec2(tile->getResolution()) / Vec2(tile->getSize());
    out_coord = ivec2(round(pixels_per_unit * bbox_min));
    out_size = ivec2(pixels_per_unit * round(bbox_max - bbox_min));
}

bool TerrainManipulator::setTerrainHeight(const LandscapeLayerMapPtr& tile, const ImagePtr& height_image)
{
    if (!tile || !height_image)
        return false;

    ImagePtr prepared_image = height_image;
    if (prepared_image->getFormat() != Image::FORMAT_RGBA32F)
    {
        if (!prepared_image->convertToFormat(Image::FORMAT_RGBA32F))
            return false;
    }

    const ivec2 tile_resolution = tile->getResolution();
    if (prepared_image->getWidth() != tile_resolution.x || prepared_image->getHeight() != tile_resolution.y)
    {
        if (!prepared_image->resize(tile_resolution.x, tile_resolution.y))
            return false;
    }

    const ImagePtr alpha_image = SurfaceRasterizer::createHeightAlphaImage(prepared_image);
    if (!alpha_image)
        return false;

    BrushOperationData operation;
    operation.height_image = prepared_image;
    operation.alpha_image = alpha_image;
    operation.modify_heights = true;

    const int operation_id = Landscape::generateOperationID();
    pending_operations_[operation_id] = operation;
    Landscape::asyncTextureDraw(operation_id,
                                tile->getGUID(),
                                ivec2_zero,
                                tile_resolution,
                                Landscape::FLAGS_FILE_DATA_HEIGHT | Landscape::FLAGS_FILE_DATA_OPACITY_HEIGHT);
    return true;
}

bool TerrainManipulator::setTerrainMask(const LandscapeLayerMapPtr& tile,
                                        const ImagePtr& mask_image,
                                        const TerrainBrushSettings& settings,
                                        int mask_index)
{
    if (!tile || !mask_image)
        return false;

    ImagePtr prepared_image = mask_image;
    if (prepared_image->getFormat() != Image::FORMAT_R8)
    {
        if (!prepared_image->convertToFormat(Image::FORMAT_R8))
            return false;
    }

    const ivec2 tile_resolution = tile->getResolution();
    if (prepared_image->getWidth() != tile_resolution.x || prepared_image->getHeight() != tile_resolution.y)
    {
        if (!prepared_image->resize(tile_resolution.x, tile_resolution.y))
            return false;
    }

    const MaterialPtr brush_material = createMaskBrush(prepared_image);
    if (!brush_material)
        return false;

    BrushOperationData operation;
    operation.brush_material = brush_material;
    operation.brush_size = static_cast<float>(std::max(1.0, settings.brush_size));
    operation.modify_mask = true;
    operation.mask_index = mask_index;

    const int mask_flags = getMaskFileDataFlags(mask_index);
    if (mask_flags == 0)
        return false;

    const int operation_id = Landscape::generateOperationID();
    pending_operations_[operation_id] = operation;
    Landscape::asyncTextureDraw(operation_id, tile->getGUID(), ivec2_zero, tile_resolution, mask_flags);
    return true;
}

bool TerrainManipulator::applyHeightOverwrite(const LandscapeTexturesPtr& buffer,
                                              const TexturePtr& height_texture,
                                              const TexturePtr& alpha_texture)
{
    if (!buffer || !height_texture || !alpha_texture)
        return false;

    const MaterialPtr brush_material = loadInheritedMaterial("terrain_brush_r32f_overwrite.basebrush",
                                                             "terrain height overwrite");
    if (!brush_material)
        return false;

    brush_material->setTexture("terrain_height", buffer->getHeight());
    brush_material->setTexture("terrain_opacity_height", buffer->getOpacityHeight());
    brush_material->setTexture("new_height", height_texture);
    brush_material->setTexture("new_alpha", alpha_texture);
    brush_material->runExpression("brush", buffer->getResolution().x, buffer->getResolution().y);
    brush_material->setTexture("terrain_height", nullptr);
    brush_material->setTexture("terrain_opacity_height", nullptr);
    brush_material->setTexture("new_height", nullptr);
    brush_material->setTexture("new_alpha", nullptr);
    return true;
}

bool TerrainManipulator::applyAlbedoOverwrite(const LandscapeTexturesPtr& buffer,
                                              const ImagePtr& albedo_image)
{
    if (!buffer || !albedo_image)
        return false;

    const MaterialPtr brush_material = loadInheritedMaterial("terrain_brush_r32f_overwrite.basebrush",
                                                             "terrain albedo overwrite");
    if (!brush_material)
        return false;

    TexturePtr albedo_texture = Texture::create();
    if (!albedo_texture || !albedo_texture->create(albedo_image))
        return false;

    brush_material->setTexture("terrain_height", buffer->getAlbedo());
    brush_material->setTexture("new_height", albedo_texture);
    brush_material->runExpression("brush", buffer->getResolution().x, buffer->getResolution().y);
    brush_material->setTexture("terrain_height", nullptr);
    brush_material->setTexture("new_height", nullptr);
    return true;
}

bool TerrainManipulator::applyBrush(const BrushOperationData& operation,
                                    const LandscapeTexturesPtr& buffer,
                                    int data_mask)
{
    if (!buffer || !operation.brush_material)
        return false;

    MaterialPtr brush_material = operation.brush_material;
    brush_material->setParameterFloat("size", operation.brush_size);
    brush_material->setParameterFloat("angle", operation.brush_rotation);
    brush_material->setParameterFloat("opacity", operation.brush_opacity);

    if (operation.modify_mask)
    {
        int available_pages = 0;
        for (int page_index = 0; page_index < 5; ++page_index)
        {
            if (buffer->getMask(page_index) && buffer->getOpacityMask(page_index))
                ++available_pages;
        }
        if (available_pages == 0)
            return false;

        for (int page_index = 0; page_index < 5; ++page_index)
        {
            const std::string mask_name = "terrain_mask_" + std::to_string(page_index);
            const std::string opacity_name = "terrain_opacity_mask_" + std::to_string(page_index);
            brush_material->setTexture(mask_name.c_str(), buffer->getMask(page_index));
            brush_material->setTexture(opacity_name.c_str(), buffer->getOpacityMask(page_index));
        }

        const int logical_mask_bit = 1 << (operation.mask_index + 2);
        brush_material->setParameterInt("data_mask", logical_mask_bit);
        brush_material->setState("data_mask", logical_mask_bit);
        brush_material->setParameterInt("data_mask_opacity", logical_mask_bit);
        brush_material->setState("data_mask_opacity", logical_mask_bit);
        brush_material->setState("masks_overide", 0);
        brush_material->runExpression("brush", buffer->getResolution().x, buffer->getResolution().y);

        for (int page_index = 0; page_index < 5; ++page_index)
        {
            const std::string mask_name = "terrain_mask_" + std::to_string(page_index);
            const std::string opacity_name = "terrain_opacity_mask_" + std::to_string(page_index);
            brush_material->setTexture(mask_name.c_str(), nullptr);
            brush_material->setTexture(opacity_name.c_str(), nullptr);
        }

        return true;
    }

    if (operation.modify_heights)
    {
        brush_material->setTexture("terrain_height", buffer->getHeight());
        brush_material->setTexture("terrain_opacity_height", buffer->getOpacityHeight());
        brush_material->setParameterFloat("height", operation.brush_height);
    }

    if (operation.modify_albedo)
        brush_material->setTexture("terrain_albedo", buffer->getAlbedo());

    brush_material->setParameterInt("data_mask", data_mask);
    brush_material->setState("height_blend_mode", static_cast<int>(operation.height_blend_mode));
    brush_material->runExpression("brush", buffer->getResolution().x, buffer->getResolution().y);
    brush_material->setTexture("terrain_height", nullptr);
    brush_material->setTexture("terrain_opacity_height", nullptr);
    brush_material->setTexture("terrain_albedo", nullptr);
    return true;
}

void TerrainManipulator::onTextureDraw(const UGUID& guid, int operation_id,
                                       const LandscapeTexturesPtr& buffer,
                                       const ivec2& coord,
                                       int data_mask)
{
    UNIGINE_UNUSED(coord);

    auto operation_it = pending_operations_.find(operation_id);
    if (operation_it == pending_operations_.end())
        return;

    const BrushOperationData operation = operation_it->second;
    pending_operations_.erase(operation_it);

    bool applied = false;
    if (operation.height_image && operation.alpha_image)
    {
        TexturePtr height_texture = Texture::create();
        TexturePtr alpha_texture = Texture::create();
        if (height_texture && alpha_texture &&
            height_texture->create(operation.height_image) &&
            alpha_texture->create(operation.alpha_image))
        {
            applied = applyHeightOverwrite(buffer, height_texture, alpha_texture);
        }
    }
    else if (operation.albedo_image)
    {
        applied = applyAlbedoOverwrite(buffer, operation.albedo_image);
    }
    else
    {
        applied = applyBrush(operation, buffer, data_mask);
    }

    if (applied)
        save_manager_.markDirty(guid);

    finalizeActionTransactionsIfIdle();
}

MaterialPtr TerrainManipulator::loadInheritedMaterial(const char* material_path, const char* log_context)
{
    const auto file_guid = FileSystem::getGUID(FileSystem::resolvePartialVirtualPath(material_path));
    if (!file_guid.isValid())
    {
        Log::warning("[TerrainManipulator] Missing %s material '%s'\n", log_context, material_path);
        return nullptr;
    }

    const auto base_material = Materials::findMaterialByFileGUID(file_guid);
    if (!base_material)
    {
        Log::warning("[TerrainManipulator] Failed to load %s material '%s'\n", log_context, material_path);
        return nullptr;
    }

    return base_material->inherit();
}

MaterialPtr TerrainManipulator::createCircularBrush(double falloff_ratio, double padding)
{
    MaterialPtr brush_material = loadInheritedMaterial("circle_medium.brush", "terrain brush");
    if (!brush_material)
        return nullptr;

    const auto opacity_texture_index = brush_material->findTexture("opacity");
    if (opacity_texture_index < 0)
        return nullptr;

    constexpr int kBrushResolution = 512;
    ImagePtr opacity_image = Image::create();
    opacity_image->create2D(kBrushResolution, kBrushResolution, Image::FORMAT_RGBA8);

    const int center_x = kBrushResolution / 2;
    const int center_y = kBrushResolution / 2;
    const float radius = (kBrushResolution - static_cast<float>(padding) * kBrushResolution) * 0.5f;

    Image::Pixel pixel;
    pixel.i.r = 255;
    pixel.i.g = 255;
    pixel.i.b = 255;
    pixel.i.a = 255;

    for (int y = 0; y < kBrushResolution; ++y)
    {
        for (int x = 0; x < kBrushResolution; ++x)
        {
            const float distance_value = std::sqrt(static_cast<float>(((x - center_x) * (x - center_x)) +
                                                                      ((y - center_y) * (y - center_y))));
            if (distance_value > radius)
            {
                pixel.i.a = 0;
            }
            else
            {
                const float normalized = clamp(distance_value / std::max(radius, 1.0f), 0.0f, 1.0f);
                if (normalized <= falloff_ratio)
                {
                    pixel.i.a = 255;
                }
                else
                {
                    const float falloff_span = std::max(1.0f - static_cast<float>(falloff_ratio), 1e-4f);
                    const float fade = 1.0f - ((normalized - falloff_ratio) / falloff_span);
                    pixel.i.a = clamp(static_cast<int>(fade * 255.0f), 0, 255);
                }
            }

            opacity_image->set2D(x, y, pixel);
        }
    }

    brush_material->setTextureImage(opacity_texture_index, opacity_image);
    return brush_material;
}

MaterialPtr TerrainManipulator::createMaskBrush(const ImagePtr& mask_image)
{
    if (!mask_image)
        return nullptr;

    MaterialPtr brush_material = loadInheritedMaterial("editor2/brushes/brush.basebrush", "mask brush");
    if (!brush_material)
        return nullptr;

    const auto opacity_texture_index = brush_material->findTexture("opacity");
    if (opacity_texture_index < 0)
        return nullptr;

    brush_material->setTextureImage(opacity_texture_index, mask_image);
    brush_material->setParameterFloat4("color", vec4(1.0f, 1.0f, 1.0f, 1.0f));
    brush_material->setParameterFloat("color_intensity", 1.0f);
    brush_material->setParameterFloat("contrast", 0.0f);
    brush_material->setState("masks_overide", 0);
    return brush_material;
}

int TerrainManipulator::getMaskFileDataFlags(int mask_index)
{
    if (mask_index < 0 || mask_index > 19)
        return 0;

    int flags = 0;
    for (int page_index = 0; page_index < 5; ++page_index)
    {
        flags |= (Landscape::FLAGS_FILE_DATA_MASK_0 << page_index);
        flags |= (Landscape::FLAGS_FILE_DATA_OPACITY_MASK_0 << page_index);
    }
    return flags;
}
