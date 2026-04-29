#define _USE_MATH_DEFINES

#include "TerrainManipulator.h"

#include <UnigineFileSystem.h>
#include <UnigineLog.h>

#include <algorithm>
#include <cmath>

using namespace Unigine;
using namespace Unigine::Math;

namespace
{
double clampPositive(double value)
{
    return std::max(0.0, value);
}

float safeFlatRatio(double flatDistance, double falloffDistance, double brushSize)
{
    const double radius = std::max(brushSize * 0.5, flatDistance + falloffDistance);
    if (radius <= Consts::EPS_D)
        return 1.0f;
    return static_cast<float>(clamp(flatDistance / radius, 0.0, 1.0));
}

float sampleSpacingForBrush(double brushSize)
{
    return static_cast<float>(clamp(brushSize * 0.2, 1.0, 10.0));
}

double effectiveFlatDistance(const TerrainBrushSettings& settings)
{
    return clampPositive(settings.flatDistance) + (clampPositive(settings.brushSize) * 0.5);
}

void logMessage(const TerrainManipulator::LogFn& log, const std::string& message)
{
    if (log)
        log(message);
}
}

TerrainManipulator::TerrainManipulator(LandscapeSaveManager& saveManager)
    : saveManager(saveManager)
{
    Landscape::getEventTextureDraw().connect(textureDrawConnection,
        [this](const UGUID& guid, int operationId, const LandscapeTexturesPtr& buffer,
               const ivec2& coord, int dataMask)
        {
            onTextureDraw(guid, operationId, buffer, coord, dataMask);
        });
}

TerrainManipulator::~TerrainManipulator()
{
    textureDrawConnection.disconnect();

    pendingOperations.clear();
    pendingTransactionCommits = 0;
    inProgress = false;
}

bool TerrainManipulator::pullTerrainToSurface(const std::vector<NodePtr>& nodes,
                                              const ObjectLandscapeTerrainPtr& terrain,
                                              const LandscapeLayerMapPtr& targetTile,
                                              const std::string& surfacePattern,
                                              const TerrainBrushSettings& settings,
                                              const LogFn& log)
{
    if (nodes.empty())
    {
        logMessage(log, "ERROR: No selected mesh nodes.");
        return false;
    }

    SurfaceRasterizer::SurfaceQuery query;
    std::string queryError;
    if (!SurfaceRasterizer::buildSurfaceQuery(surfacePattern, query, queryError))
    {
        logMessage(log, "ERROR: " + queryError);
        return false;
    }

    TerrainContext terrainContext = buildTerrainContext(terrain, targetTile);
    if (!terrainContext.terrain)
    {
        logMessage(log, "ERROR: No active landscape terrain.");
        return false;
    }

    beginActionTransaction();

    // Clean rasterization approach:
    //   1. Rasterize the mesh surface directly into a heightmap (pixels covered by mesh = target height).
    //   2. Flood-fill outward: Flat Distance = padding at full strength, Falloff Distance = smooth blend.
    // Brush Size is no longer used here (it only affected the old stamp-mode which caused ring artifacts).
    const double flatDistance = clampPositive(settings.flatDistance);
    const double falloffDistance = clampPositive(settings.falloffDistance);

    bool queuedAnyOperation = false;

    for (const auto& node : nodes)
    {
        if (!node || node->getType() != Node::OBJECT_MESH_STATIC)
            continue;

        ObjectMeshStaticPtr mesh = checked_ptr_cast<ObjectMeshStatic>(node);
        if (!mesh)
            continue;

        const std::vector<int> surfaceIds = SurfaceRasterizer::findMatchingSurfaceIds(mesh, query);
        if (surfaceIds.empty())
            continue;

        const WorldBoundBox nodeBounds = mesh->getWorldBoundBox();

        for (int surfaceId : surfaceIds)
        {
            ObjectSurface objectSurface = std::make_pair(static_ptr_cast<Object>(mesh), surfaceId);

            for (const auto& tile : terrainContext.layerMaps)
            {
                if (!tile || !tile->getWorldBoundBox().insideValid(nodeBounds))
                    continue;

                if (queueHeightRasterForTile(terrainContext,
                                             tile,
                                             objectSurface,
                                             flatDistance,
                                             falloffDistance,
                                             settings,
                                             log))
                {
                    queuedAnyOperation = true;
                }
            }
        }
    }

    finishActionScheduling();
    if (!queuedAnyOperation)
        logMessage(log, "WARNING: No terrain operations were queued.");
    return queuedAnyOperation;
}

bool TerrainManipulator::applyLandscapeMask(const std::vector<NodePtr>& nodes,
                                            const ObjectLandscapeTerrainPtr& terrain,
                                            const LandscapeLayerMapPtr& targetTile,
                                            const std::string& surfacePattern,
                                            const TerrainBrushSettings& settings,
                                            int maskIndex,
                                            const LogFn& log)
{
    if (nodes.empty())
    {
        logMessage(log, "ERROR: No selected mesh nodes.");
        return false;
    }

    SurfaceRasterizer::SurfaceQuery query;
    std::string queryError;
    if (!SurfaceRasterizer::buildSurfaceQuery(surfacePattern, query, queryError))
    {
        logMessage(log, "ERROR: " + queryError);
        return false;
    }

    TerrainContext terrainContext = buildTerrainContext(terrain, targetTile);
    if (!terrainContext.terrain)
    {
        logMessage(log, "ERROR: No active landscape terrain.");
        return false;
    }

    beginActionTransaction();
    bool queuedAnyOperation = false;
    const double maskFlatDistance = effectiveFlatDistance(settings);

    for (const auto& node : nodes)
    {
        if (!node || node->getType() != Node::OBJECT_MESH_STATIC)
            continue;

        ObjectMeshStaticPtr mesh = checked_ptr_cast<ObjectMeshStatic>(node);
        if (!mesh)
            continue;

        const std::vector<int> surfaceIds = SurfaceRasterizer::findMatchingSurfaceIds(mesh, query);
        if (surfaceIds.empty())
            continue;

        const WorldBoundBox nodeBounds = mesh->getWorldBoundBox();
        for (int surfaceId : surfaceIds)
        {
            ObjectSurface objectSurface = std::make_pair(static_ptr_cast<Object>(mesh), surfaceId);
            SurfaceRasterizer::RasterBuffer rasterBuffer;

            for (const auto& tile : terrainContext.layerMaps)
            {
                if (!tile || !tile->getWorldBoundBox().insideValid(nodeBounds))
                    continue;

                if (!SurfaceRasterizer::rasterizeSurfaceMask(tile, objectSurface, rasterBuffer))
                    continue;

                SurfaceRasterizer::applyDistanceFalloff(tile,
                                                        rasterBuffer,
                                                        maskFlatDistance,
                                                        settings.falloffDistance);

                const ImagePtr maskImage = SurfaceRasterizer::createMaskImage(rasterBuffer);
                if (maskImage && setTerrainMask(tile, maskImage, settings, maskIndex))
                    queuedAnyOperation = true;
            }
        }
    }

    finishActionScheduling();
    if (!queuedAnyOperation)
        logMessage(log, "WARNING: No landscape mask operations were queued.");
    return queuedAnyOperation;
}

bool TerrainManipulator::resetTerrainHeights(const std::vector<NodePtr>& nodes,
                                             const ObjectLandscapeTerrainPtr& terrain,
                                             const LandscapeLayerMapPtr& targetTile,
                                             const LogFn& log)
{
    if (nodes.empty())
    {
        logMessage(log, "ERROR: No selected mesh nodes.");
        return false;
    }

    TerrainContext terrainContext = buildTerrainContext(terrain, targetTile);
    if (!terrainContext.terrain)
    {
        logMessage(log, "ERROR: No active landscape terrain.");
        return false;
    }

    beginActionTransaction();
    bool queuedAnyOperation = false;

    for (const auto& node : nodes)
    {
        if (!node || node->getType() != Node::OBJECT_MESH_STATIC)
            continue;

        ObjectMeshStaticPtr mesh = checked_ptr_cast<ObjectMeshStatic>(node);
        if (!mesh)
            continue;

        for (const auto& tile : terrainContext.layerMaps)
        {
            if (!tile || !tile->getWorldBoundBox().insideValid(mesh->getWorldBoundBox()))
                continue;

            const ivec2 tileResolution = tile->getResolution();
            ImagePtr heightImage = Image::create();
            heightImage->create2D(tileResolution.x, tileResolution.y, Image::FORMAT_RGBA32F);

            Image::Pixel pixel;
            pixel.f.r = 0.0f;
            pixel.f.g = 0.0f;
            pixel.f.b = 0.0f;
            pixel.f.a = 1.0f;

            for (int x = 0; x < tileResolution.x; ++x)
            {
                for (int y = 0; y < tileResolution.y; ++y)
                    heightImage->set2D(x, y, pixel);
            }

            if (setTerrainHeight(tile, heightImage))
                queuedAnyOperation = true;
        }
    }

    finishActionScheduling();
    if (!queuedAnyOperation)
        logMessage(log, "WARNING: No terrain tiles intersected the selected meshes.");
    return queuedAnyOperation;
}

bool TerrainManipulator::paintWhiteHeight(const std::vector<NodePtr>& nodes,
                                          const ObjectLandscapeTerrainPtr& terrain,
                                          const LandscapeLayerMapPtr& targetTile,
                                          const TerrainBrushSettings& settings,
                                          const LogFn& log)
{
    if (nodes.empty())
    {
        logMessage(log, "ERROR: No selected mesh nodes.");
        return false;
    }

    TerrainContext terrainContext = buildTerrainContext(terrain, targetTile);
    if (!terrainContext.terrain)
    {
        logMessage(log, "ERROR: No active landscape terrain.");
        return false;
    }

    beginActionTransaction();
    bool queuedAnyOperation = false;

    for (const auto& node : nodes)
    {
        if (!node || node->getType() != Node::OBJECT_MESH_STATIC)
            continue;

        ObjectMeshStaticPtr mesh = checked_ptr_cast<ObjectMeshStatic>(node);
        if (!mesh)
            continue;

        // Use the mesh's top Z in world space as the uniform erase height
        const WorldBoundBox meshBounds = mesh->getWorldBoundBox();
        const float paintHeight = static_cast<float>(meshBounds.maximum.z);

        logMessage(log, "  Erasing entire tile to height = " + std::to_string(paintHeight) + " m (mesh top).");

        for (const auto& tile : terrainContext.layerMaps)
        {
            if (!tile || !tile->getWorldBoundBox().insideValid(meshBounds))
                continue;

            const ivec2 tileResolution = tile->getResolution();
            ImagePtr heightImage = Image::create();
            heightImage->create2D(tileResolution.x, tileResolution.y, Image::FORMAT_RGBA32F);

            Image::Pixel pixel;
            pixel.f.r = paintHeight;
            pixel.f.g = paintHeight;
            pixel.f.b = paintHeight;
            pixel.f.a = 1.0f;  // Full opacity: overwrite entire tile.

            for (int x = 0; x < tileResolution.x; ++x)
            {
                for (int y = 0; y < tileResolution.y; ++y)
                    heightImage->set2D(x, y, pixel);
            }

            if (setTerrainHeight(tile, heightImage))
                queuedAnyOperation = true;
        }
    }

    finishActionScheduling();
    if (!queuedAnyOperation)
        logMessage(log, "WARNING: No terrain tiles intersected the selected meshes.");
    return queuedAnyOperation;
}

bool TerrainManipulator::isBusy() const
{
    return inProgress || !pendingOperations.empty();
}

size_t TerrainManipulator::pendingOperationCount() const
{
    return pendingOperations.size();
}

void TerrainManipulator::flushPendingSaves()
{
    saveManager.forceFlush();
}

bool TerrainManipulator::beginActionTransaction()
{
    saveManager.beginTransaction();
    return true;
}

void TerrainManipulator::finishActionScheduling()
{
    if (pendingOperations.empty())
    {
        saveManager.endTransaction();
        return;
    }

    ++pendingTransactionCommits;
    inProgress = true;
}

void TerrainManipulator::EndTransactionsIfIdle()
{
    if (!pendingOperations.empty())
        return;

    while (pendingTransactionCommits > 0)
    {
        saveManager.endTransaction();
        --pendingTransactionCommits;
    }

    inProgress = false;
}

TerrainManipulator::TerrainContext TerrainManipulator::buildTerrainContext(const ObjectLandscapeTerrainPtr& terrain,
                                                                          const LandscapeLayerMapPtr& targetTile)
{
    TerrainContext context;
    context.terrain = terrain ? terrain : Landscape::getActiveTerrain();
    context.fetch = LandscapeFetch::create();
    if (context.fetch)
        context.fetch->setUsesHeight(true);
    if (!context.terrain)
        return context;

    if (targetTile)
    {
        context.layerMaps.push_back(targetTile);
        return context;
    }

    context.layerMaps.reserve(context.terrain->getNumChildren());
    for (int childIndex = 0; childIndex < context.terrain->getNumChildren(); ++childIndex)
    {
        LandscapeLayerMapPtr tile = checked_ptr_cast<LandscapeLayerMap>(context.terrain->getChild(childIndex));
        if (tile)
            context.layerMaps.push_back(tile);
    }

    return context;
}

bool TerrainManipulator::queueHeightRasterForTile(const TerrainContext& terrainContext,
                                                  const LandscapeLayerMapPtr& tile,
                                                  const ObjectSurface& objectSurface,
                                                  double flatDistance,
                                                  double falloffDistance,
                                                  const TerrainBrushSettings& settings,
                                                  const LogFn& log)
{
    SurfaceRasterizer::RasterBuffer rasterBuffer;
    if (!SurfaceRasterizer::rasterizeSurfaceHeight(tile, objectSurface, rasterBuffer))
    {
        logMessage(log, "  Tile '" + std::string(tile->getName()) + "': no pixels covered by mesh surface.");
        return false;
    }

    logMessage(log, "  Tile '" + std::string(tile->getName()) + "': rasterized "
                    + std::to_string(rasterBuffer.seeds.size()) + " seed pixels.");

    SurfaceRasterizer::applyDistanceFalloff(tile,
                                            rasterBuffer,
                                            flatDistance,
                                            falloffDistance);

    // Blend on the CPU because the overwrite brush cannot sample terrain_height itself.
    SurfaceRasterizer::blendFalloffWithExistingTerrain(tile,
                                                       terrainContext.fetch,
                                                       rasterBuffer,
                                                       settings.clampToOriginal);
    SurfaceRasterizer::fillUnpaintedPixelsWithTerrain(tile,
                                                       terrainContext.fetch,
                                                       rasterBuffer);

    const ImagePtr heightImage = SurfaceRasterizer::createHeightImage(rasterBuffer);
    if (!heightImage)
        return false;

    saveDebugRasterImages(tile, rasterBuffer, log);
    return setTerrainHeight(tile, heightImage);
}

void TerrainManipulator::saveDebugRasterImages(const LandscapeLayerMapPtr& tile,
                                               const SurfaceRasterizer::RasterBuffer& rasterBuffer,
                                               const LogFn& log)
{
    if (!kSaveDebugRasterImages || !tile)
        return;

    const std::string tileName = std::string(tile->getName());
    const std::string heightPath = std::string(kDebugRasterOutputDir) + "/debug_height_" + tileName + ".png";
    const std::string alphaPath = std::string(kDebugRasterOutputDir) + "/debug_alpha_" + tileName + ".png";
    const ivec2 resolution = rasterBuffer.resolution;

    float minHeight = 1e30f;
    float maxHeight = -1e30f;
    for (float value : rasterBuffer.values)
    {
        if (value == 0.0f)
            continue;

        minHeight = std::min(minHeight, value);
        maxHeight = std::max(maxHeight, value);
    }
    if (maxHeight <= minHeight)
        maxHeight = minHeight + 1.0f;

    ImagePtr heightPreview = Image::create();
    heightPreview->create2D(resolution.x, resolution.y, Image::FORMAT_R8);
    ImagePtr alphaPreview = Image::create();
    alphaPreview->create2D(resolution.x, resolution.y, Image::FORMAT_R8);

    for (int y = 0; y < resolution.y; ++y)
    {
        for (int x = 0; x < resolution.x; ++x)
        {
            const int index = x + (y * resolution.x);
            const int heightValue = static_cast<int>(
                clamp((rasterBuffer.values[index] - minHeight) / (maxHeight - minHeight), 0.0f, 1.0f) * 255.0f);
            const int alphaValue = static_cast<int>(clamp(rasterBuffer.alpha[index], 0.0f, 1.0f) * 255.0f);
            heightPreview->set2D(x, y, Image::Pixel(heightValue, heightValue, heightValue, 255));
            alphaPreview->set2D(x, y, Image::Pixel(alphaValue, alphaValue, alphaValue, 255));
        }
    }

    heightPreview->save(heightPath.c_str());
    alphaPreview->save(alphaPath.c_str());
    logMessage(log, "  DEBUG: saved " + heightPath + " and " + alphaPath);
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
    LandscapeLayerMapPtr bestMatch = nullptr;
    for (const auto& tile : context.layerMaps)
    {
        if (!tile || !tile->getWorldBoundBox().inside(position))
            continue;
        if (!bestMatch || tile->getOrder() > bestMatch->getOrder())
            bestMatch = tile;
    }
    return bestMatch;
}

void TerrainManipulator::raiseTerrainAtPoint(const TerrainContext& context,
                                             const dvec3& point,
                                             const dvec2& brushSize,
                                             const MaterialPtr& brushMaterial,
                                             float brushRotation,
                                             bool targetAlbedo,
                                             LandscapeLayerMapPtr forcedTile)
{
    TerrainContext mutableContext = context;
    if (!terrainAvailable(mutableContext, dvec2(point.x, point.y)))
        return;

    LandscapeLayerMapPtr tile = forcedTile ? forcedTile : findContainingLayerMap(context, point);
    if (!tile)
        return;

    const dvec3 brushLocalPosition = tile->getIWorldTransform() * point;
    const quat brushWorldRotation = quat(vec3_up, brushRotation);
    const quat brushLocalRotation = brushWorldRotation * inverse(tile->getWorldRotation());
    const dvec2 halfSize = brushSize * 0.5;

    ivec2 drawingCoord;
    ivec2 drawingSize;
    calculateDrawingRegion(brushLocalPosition, brushLocalRotation, halfSize, tile, drawingCoord, drawingSize);

    drawingCoord.x = clamp(drawingCoord.x, 0, tile->getResolution().x - 1);
    drawingCoord.y = clamp(drawingCoord.y, 0, tile->getResolution().y - 1);
    drawingSize.x = clamp(drawingSize.x, 1, tile->getResolution().x - drawingCoord.x);
    drawingSize.y = clamp(drawingSize.y, 1, tile->getResolution().y - drawingCoord.y);

    BrushOperationData operation;
    operation.brushMaterial = brushMaterial;
    operation.brushHeight = static_cast<float>(point.z);
    operation.brushSize = static_cast<float>(std::max(drawingSize.x, drawingSize.y));
    operation.brushRotation = brushRotation;
    operation.modifyHeights = !targetAlbedo;
    operation.modifyAlbedo = targetAlbedo;

    const int flags = targetAlbedo
        ? Landscape::FLAGS_FILE_DATA_ALBEDO
        : (Landscape::FLAGS_FILE_DATA_HEIGHT | Landscape::FLAGS_FILE_DATA_OPACITY_HEIGHT);

    const int operationId = Landscape::generateOperationID();
    pendingOperations[operationId] = operation;
    Landscape::asyncTextureDraw(operationId, tile->getGUID(), drawingCoord, drawingSize, flags);

    if (forcedTile)
        return;

    const int maxX = tile->getResolution().x - drawingSize.x;
    const int maxY = tile->getResolution().y - drawingSize.y;
    if ((drawingCoord.x > 0 && drawingCoord.x < maxX) &&
        (drawingCoord.y > 0 && drawingCoord.y < maxY))
    {
        return;
    }

    dvec3 boundaryProbe = point;
    const dvec3 tilePosition = tile->getWorldPosition();
    boundaryProbe.x = (drawingCoord.x <= 0)
        ? tilePosition.x + drawingCoord.x
        : tilePosition.x + drawingCoord.x + drawingSize.x;
    boundaryProbe.y = (drawingCoord.y <= 0)
        ? tilePosition.y + drawingCoord.y
        : tilePosition.y + drawingCoord.y + drawingSize.y;

    LandscapeLayerMapPtr neighborTile = findContainingLayerMap(context, boundaryProbe);
    if (!neighborTile || neighborTile == tile)
        return;

    raiseTerrainAtPoint(context, point, brushSize, brushMaterial, brushRotation, targetAlbedo, neighborTile);
}

void TerrainManipulator::calculateDrawingRegion(const dvec3& brushLocalPosition,
                                                const quat& brushLocalRotation,
                                                const dvec2& halfSize,
                                                const LandscapeLayerMapPtr& tile,
                                                ivec2& outCoord,
                                                ivec2& outSize)
{
    const Vec3 corners[4] = {
        brushLocalPosition + brushLocalRotation * Vec3(-halfSize.x, -halfSize.y, 0.0),
        brushLocalPosition + brushLocalRotation * Vec3( halfSize.x, -halfSize.y, 0.0),
        brushLocalPosition + brushLocalRotation * Vec3(-halfSize.x,  halfSize.y, 0.0),
        brushLocalPosition + brushLocalRotation * Vec3( halfSize.x,  halfSize.y, 0.0),
    };

    const Vec2 bboxMin(
        std::min(std::min(corners[0].x, corners[1].x), std::min(corners[2].x, corners[3].x)),
        std::min(std::min(corners[0].y, corners[1].y), std::min(corners[2].y, corners[3].y)));
    const Vec2 bboxMax(
        std::max(std::max(corners[0].x, corners[1].x), std::max(corners[2].x, corners[3].x)),
        std::max(std::max(corners[0].y, corners[1].y), std::max(corners[2].y, corners[3].y)));

    const Vec2 pixelsPerUnit = Vec2(tile->getResolution()) / Vec2(tile->getSize());
    outCoord = ivec2(round(pixelsPerUnit * bboxMin));
    outSize = ivec2(pixelsPerUnit * round(bboxMax - bboxMin));
}

bool TerrainManipulator::setTerrainHeight(const LandscapeLayerMapPtr& tile, const ImagePtr& heightImage)
{
    if (!tile || !heightImage)
        return false;

    ImagePtr preparedImage = heightImage;
    if (preparedImage->getFormat() != Image::FORMAT_RGBA32F)
    {
        if (!preparedImage->convertToFormat(Image::FORMAT_RGBA32F))
            return false;
    }

    const ivec2 tileResolution = tile->getResolution();
    if (preparedImage->getWidth() != tileResolution.x || preparedImage->getHeight() != tileResolution.y)
    {
        if (!preparedImage->resize(tileResolution.x, tileResolution.y))
            return false;
    }

    const ImagePtr alphaImage = SurfaceRasterizer::createHeightAlphaImage(preparedImage);
    if (!alphaImage)
        return false;

    BrushOperationData operation;
    operation.heightImage = preparedImage;
    operation.alphaImage = alphaImage;
    operation.modifyHeights = true;

    const int operationId = Landscape::generateOperationID();
    pendingOperations[operationId] = operation;
    Landscape::asyncTextureDraw(operationId,
                                tile->getGUID(),
                                ivec2_zero,
                                tileResolution,
                                Landscape::FLAGS_FILE_DATA_HEIGHT | Landscape::FLAGS_FILE_DATA_OPACITY_HEIGHT);
    return true;
}

bool TerrainManipulator::setTerrainMask(const LandscapeLayerMapPtr& tile,
                                        const ImagePtr& maskImage,
                                        const TerrainBrushSettings& settings,
                                        int maskIndex)
{
    if (!tile || !maskImage)
        return false;

    ImagePtr preparedImage = maskImage;
    if (preparedImage->getFormat() != Image::FORMAT_R8)
    {
        if (!preparedImage->convertToFormat(Image::FORMAT_R8))
            return false;
    }

    const ivec2 tileResolution = tile->getResolution();
    if (preparedImage->getWidth() != tileResolution.x || preparedImage->getHeight() != tileResolution.y)
    {
        if (!preparedImage->resize(tileResolution.x, tileResolution.y))
            return false;
    }

    const MaterialPtr brushMaterial = createMaskBrush(preparedImage);
    if (!brushMaterial)
        return false;

    BrushOperationData operation;
    operation.brushMaterial = brushMaterial;
    operation.brushSize = static_cast<float>(std::max(1.0, settings.brushSize));
    operation.modifyMask = true;
    operation.maskIndex = maskIndex;

    const int maskFlags = getMaskFileDataFlags(maskIndex);
    if (maskFlags == 0)
        return false;

    const int operationId = Landscape::generateOperationID();
    pendingOperations[operationId] = operation;
    Landscape::asyncTextureDraw(operationId, tile->getGUID(), ivec2_zero, tileResolution, maskFlags);
    return true;
}

bool TerrainManipulator::applyHeightOverwrite(const LandscapeTexturesPtr& buffer,
                                              const TexturePtr& heightTexture,
                                              const TexturePtr& alphaTexture)
{
    if (!buffer || !heightTexture || !alphaTexture)
        return false;

    const MaterialPtr brushMaterial = loadInheritedMaterial("terrain_brush_r32f_overwrite.basebrush",
                                                             "terrain height overwrite");
    if (!brushMaterial)
        return false;

    brushMaterial->setTexture("terrain_height", buffer->getHeight());
    brushMaterial->setTexture("terrain_opacity_height", buffer->getOpacityHeight());
    brushMaterial->setTexture("new_height", heightTexture);
    brushMaterial->setTexture("new_alpha", alphaTexture);
    brushMaterial->runExpression("brush", buffer->getResolution().x, buffer->getResolution().y);
    clearBrushMaterialTextures(brushMaterial);
    brushMaterial->setTexture("new_height", nullptr);
    brushMaterial->setTexture("new_alpha", nullptr);
    return true;
}

bool TerrainManipulator::applyAlbedoOverwrite(const LandscapeTexturesPtr& buffer,
                                              const ImagePtr& albedoImage)
{
    if (!buffer || !albedoImage)
        return false;

    const MaterialPtr brushMaterial = loadInheritedMaterial("terrain_brush_r32f_overwrite.basebrush",
                                                             "terrain albedo overwrite");
    if (!brushMaterial)
        return false;

    TexturePtr albedoTexture = Texture::create();
    if (!albedoTexture || !albedoTexture->create(albedoImage))
        return false;

    brushMaterial->setTexture("terrain_height", buffer->getAlbedo());
    brushMaterial->setTexture("new_height", albedoTexture);
    brushMaterial->runExpression("brush", buffer->getResolution().x, buffer->getResolution().y);
    clearBrushMaterialTextures(brushMaterial);
    brushMaterial->setTexture("new_height", nullptr);
    return true;
}

bool TerrainManipulator::applyBrush(const BrushOperationData& operation,
                                    const LandscapeTexturesPtr& buffer,
                                    int dataMask)
{
    if (!buffer || !operation.brushMaterial)
        return false;

    MaterialPtr brushMaterial = operation.brushMaterial;
    brushMaterial->setParameterFloat("size", operation.brushSize);
    brushMaterial->setParameterFloat("angle", operation.brushRotation);
    brushMaterial->setParameterFloat("opacity", operation.brushOpacity);

    if (operation.modifyMask)
        return applyMaskBrush(operation, buffer);

    if (operation.modifyHeights)
    {
        brushMaterial->setTexture("terrain_height", buffer->getHeight());
        brushMaterial->setTexture("terrain_opacity_height", buffer->getOpacityHeight());
        brushMaterial->setParameterFloat("height", operation.brushHeight);
    }

    if (operation.modifyAlbedo)
        brushMaterial->setTexture("terrain_albedo", buffer->getAlbedo());

    brushMaterial->setParameterInt("data_mask", dataMask);
    brushMaterial->setState("height_blend_mode", static_cast<int>(operation.heightBlendMode));
    brushMaterial->runExpression("brush", buffer->getResolution().x, buffer->getResolution().y);
    clearBrushMaterialTextures(brushMaterial);
    return true;
}

bool TerrainManipulator::applyMaskBrush(const BrushOperationData& operation,
                                        const LandscapeTexturesPtr& buffer)
{
    if (!buffer || !operation.brushMaterial)
        return false;

    MaterialPtr brushMaterial = operation.brushMaterial;
    int availablePages = 0;
    for (int pageIndex = 0; pageIndex < kLandscapeMaskPageCount; ++pageIndex)
    {
        if (buffer->getMask(pageIndex) && buffer->getOpacityMask(pageIndex))
            ++availablePages;
    }
    if (availablePages == 0)
        return false;

    for (int pageIndex = 0; pageIndex < kLandscapeMaskPageCount; ++pageIndex)
    {
        const std::string maskName = "terrain_mask_" + std::to_string(pageIndex);
        const std::string opacityName = "terrain_opacity_mask_" + std::to_string(pageIndex);
        brushMaterial->setTexture(maskName.c_str(), buffer->getMask(pageIndex));
        brushMaterial->setTexture(opacityName.c_str(), buffer->getOpacityMask(pageIndex));
    }

    // Unigine reserves the first two data-mask bits for height data; landscape masks start after them.
    const int logicalMaskBit = 1 << (operation.maskIndex + kMaskDataBitOffset);
    brushMaterial->setParameterInt("data_mask", logicalMaskBit);
    brushMaterial->setState("data_mask", logicalMaskBit);
    brushMaterial->setParameterInt("data_mask_opacity", logicalMaskBit);
    brushMaterial->setState("data_mask_opacity", logicalMaskBit);
    brushMaterial->setState("masks_overide", 0);
    brushMaterial->runExpression("brush", buffer->getResolution().x, buffer->getResolution().y);

    for (int pageIndex = 0; pageIndex < kLandscapeMaskPageCount; ++pageIndex)
    {
        const std::string maskName = "terrain_mask_" + std::to_string(pageIndex);
        const std::string opacityName = "terrain_opacity_mask_" + std::to_string(pageIndex);
        brushMaterial->setTexture(maskName.c_str(), nullptr);
        brushMaterial->setTexture(opacityName.c_str(), nullptr);
    }

    return true;
}

void TerrainManipulator::onTextureDraw(const UGUID& guid, int operationId,
                                       const LandscapeTexturesPtr& buffer,
                                       const ivec2& coord,
                                       int dataMask)
{
    UNIGINE_UNUSED(coord);

    auto operationIt = pendingOperations.find(operationId);
    if (operationIt == pendingOperations.end())
        return;

    const BrushOperationData operation = operationIt->second;
    pendingOperations.erase(operationIt);

    bool applied = false;
    if (operation.heightImage && operation.alphaImage)
    {
        TexturePtr heightTexture = Texture::create();
        TexturePtr alphaTexture = Texture::create();
        if (heightTexture && alphaTexture &&
            heightTexture->create(operation.heightImage) &&
            alphaTexture->create(operation.alphaImage))
        {
            applied = applyHeightOverwrite(buffer, heightTexture, alphaTexture);
        }
    }
    else if (operation.albedoImage)
    {
        applied = applyAlbedoOverwrite(buffer, operation.albedoImage);
    }
    else
    {
        applied = applyBrush(operation, buffer, dataMask);
    }

    if (applied)
        saveManager.markDirty(guid);

    EndTransactionsIfIdle();
}

MaterialPtr TerrainManipulator::loadInheritedMaterial(const char* materialPath, const char* logContext)
{
    const auto fileGuid = FileSystem::getGUID(FileSystem::resolvePartialVirtualPath(materialPath));
    if (!fileGuid.isValid())
    {
        Log::error("[TerrainManipulator] Missing %s material '%s'\n", logContext, materialPath);
        return nullptr;
    }

    const auto baseMaterial = Materials::findMaterialByFileGUID(fileGuid);
    if (!baseMaterial)
    {
        Log::error("[TerrainManipulator] Failed to load %s material '%s'\n", logContext, materialPath);
        return nullptr;
    }

    return baseMaterial->inherit();
}

void TerrainManipulator::clearBrushMaterialTextures(const MaterialPtr& brushMaterial)
{
    if (!brushMaterial)
        return;

    brushMaterial->setTexture("terrain_height", nullptr);
    brushMaterial->setTexture("terrain_opacity_height", nullptr);
    brushMaterial->setTexture("terrain_albedo", nullptr);
}

MaterialPtr TerrainManipulator::createCircularBrush(double falloffRatio, double padding)
{
    MaterialPtr brushMaterial = loadInheritedMaterial("circle_medium.brush", "terrain brush");
    if (!brushMaterial)
        return nullptr;

    const auto opacityTextureIndex = brushMaterial->findTexture("opacity");
    if (opacityTextureIndex < 0)
        return nullptr;

    // 512 matches the editor brush opacity texture size and keeps falloff edges smooth.
    constexpr int kBrushResolution = 512;
    ImagePtr opacityImage = Image::create();
    opacityImage->create2D(kBrushResolution, kBrushResolution, Image::FORMAT_RGBA8);

    const int centerX = kBrushResolution / 2;
    const int centerY = kBrushResolution / 2;
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
            const float distanceValue = std::sqrt(static_cast<float>(((x - centerX) * (x - centerX)) +
                                                                      ((y - centerY) * (y - centerY))));
            if (distanceValue > radius)
            {
                pixel.i.a = 0;
            }
            else
            {
                const float normalized = clamp(distanceValue / std::max(radius, 1.0f), 0.0f, 1.0f);
                if (normalized <= falloffRatio)
                {
                    pixel.i.a = 255;
                }
                else
                {
                    const float falloffSpan = std::max(1.0f - static_cast<float>(falloffRatio), 1e-4f);
                    const float fade = 1.0f - ((normalized - falloffRatio) / falloffSpan);
                    pixel.i.a = clamp(static_cast<int>(fade * 255.0f), 0, 255);
                }
            }

            opacityImage->set2D(x, y, pixel);
        }
    }

    brushMaterial->setTextureImage(opacityTextureIndex, opacityImage);
    return brushMaterial;
}

MaterialPtr TerrainManipulator::createMaskBrush(const ImagePtr& maskImage)
{
    if (!maskImage)
        return nullptr;

    MaterialPtr brushMaterial = loadInheritedMaterial("editor2/brushes/brush.basebrush", "mask brush");
    if (!brushMaterial)
        return nullptr;

    const auto opacityTextureIndex = brushMaterial->findTexture("opacity");
    if (opacityTextureIndex < 0)
        return nullptr;

    brushMaterial->setTextureImage(opacityTextureIndex, maskImage);
    brushMaterial->setParameterFloat4("color", vec4(1.0f, 1.0f, 1.0f, 1.0f));
    brushMaterial->setParameterFloat("color_intensity", 1.0f);
    brushMaterial->setParameterFloat("contrast", 0.0f);
    brushMaterial->setState("masks_overide", 0);
    return brushMaterial;
}

int TerrainManipulator::getMaskFileDataFlags(int maskIndex)
{
    if (maskIndex < 0 || maskIndex > kMaxLandscapeMaskIndex)
        return 0;

    int flags = 0;
    for (int pageIndex = 0; pageIndex < kLandscapeMaskPageCount; ++pageIndex)
    {
        flags |= (Landscape::FLAGS_FILE_DATA_MASK_0 << pageIndex);
        flags |= (Landscape::FLAGS_FILE_DATA_OPACITY_MASK_0 << pageIndex);
    }
    return flags;
}
