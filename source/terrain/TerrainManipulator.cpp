#include "TerrainManipulator.h"
#include "TerrainRasterPlanner.h"

#include <UnigineFileSystem.h>
#include <UnigineLog.h>

#include <algorithm>

using namespace Unigine;
using namespace Unigine::Math;

namespace
{
double clampPositive(double value)
{
    return std::max(0.0, value);
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

    (void)beginActionTransaction();

    // Clean rasterization approach:
    //   1. Rasterize the mesh surface directly into a heightmap (pixels covered by mesh = target height).
    //   2. Flood-fill outward: Flat Distance = padding at full strength, Falloff Distance = smooth blend.
    // Brush Size is no longer used here (it only affected the old stamp-mode which caused ring artifacts).
    const double flatDistance = clampPositive(settings.flatDistance);
    const double falloffDistance = clampPositive(settings.falloffDistance);

    logMessage(log, "  Settings — Flat Distance: " + std::to_string(flatDistance)
                    + " m, Falloff Distance: " + std::to_string(falloffDistance) + " m");

    bool queuedAnyOperation = false;

    std::vector<TerrainRasterPlanner::TileRasterPlan> plans =
        TerrainRasterPlanner::buildHeightPlans(nodes, terrainContext.layerMaps, query);
    for (auto& plan : plans)
    {
        logMessage(log, "  Tile '" + std::string(plan.tile->getName()) + "': rasterized "
                        + std::to_string(plan.mergedSurfaceCount) + " selected surface(s) together.");

        if (queueHeightRasterForTile(terrainContext,
                                     plan.tile,
                                     plan.rasterBuffer,
                                     flatDistance,
                                     falloffDistance,
                                     settings,
                                     log))
        {
            queuedAnyOperation = true;
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

    (void)beginActionTransaction();
    bool queuedAnyOperation = false;

    logMessage(log, "  Settings — Flat Distance: " + std::to_string(clampPositive(settings.flatDistance))
                    + " m, Falloff Distance: " + std::to_string(clampPositive(settings.falloffDistance)) + " m");

    std::vector<TerrainRasterPlanner::TileRasterPlan> plans =
        TerrainRasterPlanner::buildMaskPlans(nodes, terrainContext.layerMaps, query);
    for (auto& plan : plans)
    {
        if (queueMaskRasterForTile(plan.tile, plan.rasterBuffer, settings, maskIndex, log))
        {
            logMessage(log, "  Tile '" + std::string(plan.tile->getName()) + "': rasterized "
                            + std::to_string(plan.mergedSurfaceCount) + " selected surface(s) into one mask operation.");
            queuedAnyOperation = true;
        }
    }

    finishActionScheduling();
    if (!queuedAnyOperation)
        logMessage(log, "WARNING: No landscape mask operations were queued.");
    return queuedAnyOperation;
}

bool TerrainManipulator::paintWhiteHeight(const std::vector<NodePtr>& nodes,
                                          const ObjectLandscapeTerrainPtr& terrain,
                                          const LandscapeLayerMapPtr& targetTile,
                                          const TerrainBrushSettings& settings,
                                          const LogFn& log)
{
    // nodes and settings are intentionally unused here: this operation erases every
    // tile of the selected terrain (or targetTile if specified), not just those
    // under the selected meshes. The parameters are kept for API consistency.
    (void)nodes;
    (void)settings;

    TerrainContext terrainContext = buildTerrainContext(terrain, targetTile);
    if (!terrainContext.terrain)
    {
        logMessage(log, "ERROR: No active landscape terrain.");
        return false;
    }

    (void)beginActionTransaction();
    bool queuedAnyOperation = false;
    constexpr float kErasedHeight = 0.0f;

    const std::string scope = targetTile ? ("tile '" + std::string(targetTile->getName()) + "'") : "all tiles";
    logMessage(log, "  Erasing terrain heightmap data to 0.0 m on " + scope + ".");

    for (const auto& tile : terrainContext.layerMaps)
    {
        if (!tile)
            continue;

        const ImagePtr heightImage = createSolidHeightImage(tile->getResolution(), kErasedHeight);
        if (setTerrainHeight(tile, heightImage))
            queuedAnyOperation = true;
    }

    finishActionScheduling();
    if (!queuedAnyOperation)
        logMessage(log, "WARNING: No terrain heightmap data was queued for erase.");
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

void TerrainManipulator::endTransactionsIfIdle()
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
                                                  SurfaceRasterizer::RasterBuffer& rasterBuffer,
                                                  double flatDistance,
                                                  double falloffDistance,
                                                  const TerrainBrushSettings& settings,
                                                  const LogFn& log)
{
    if (rasterBuffer.empty())
    {
        logMessage(log, "WARNING: Raster buffer is empty for tile '" + std::string(tile ? tile->getName() : "null") + "' — no mesh triangles were rasterized.");
        return false;
    }

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

    const SurfaceRasterizer::RasterRegion region = SurfaceRasterizer::calculateTouchedRegion(rasterBuffer);
    const ImagePtr heightImage = SurfaceRasterizer::createHeightImage(rasterBuffer, region);
    if (!heightImage)
    {
        logMessage(log, "ERROR: Failed to create height image for tile '" + std::string(tile ? tile->getName() : "null") + "'.");
        return false;
    }

    saveDebugRasterImages(tile, rasterBuffer, log);
    return setTerrainHeight(tile, heightImage, region);
}

bool TerrainManipulator::queueMaskRasterForTile(const LandscapeLayerMapPtr& tile,
                                                SurfaceRasterizer::RasterBuffer& rasterBuffer,
                                                const TerrainBrushSettings& settings,
                                                int maskIndex,
                                                const LogFn& log)
{
    if (rasterBuffer.empty())
    {
        logMessage(log, "WARNING: Raster buffer is empty for tile '" + std::string(tile ? tile->getName() : "null") + "' — no mesh triangles were rasterized.");
        return false;
    }

    const double maskFlatDistance = clampPositive(settings.flatDistance);
    SurfaceRasterizer::applyDistanceFalloff(tile, rasterBuffer, maskFlatDistance, settings.falloffDistance);

    const SurfaceRasterizer::RasterRegion region = SurfaceRasterizer::calculateTouchedRegion(rasterBuffer);
    const ImagePtr maskImage = SurfaceRasterizer::createMaskImage(rasterBuffer, region);
    if (!maskImage)
    {
        logMessage(log, "ERROR: Failed to create mask image for tile '" + std::string(tile ? tile->getName() : "null") + "'.");
        return false;
    }

    return setTerrainMask(tile, maskImage, settings, maskIndex, region);
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

bool TerrainManipulator::setTerrainHeight(const LandscapeLayerMapPtr& tile,
                                          const ImagePtr& heightImage,
                                          const SurfaceRasterizer::RasterRegion& region)
{
    if (!tile || !heightImage)
    {
        Log::error("[TerrainManipulator] setTerrainHeight: null tile or height image.\n");
        return false;
    }

    ImagePtr preparedImage = heightImage;
    if (preparedImage->getFormat() != Image::FORMAT_RGBA32F)
    {
        if (!preparedImage->convertToFormat(Image::FORMAT_RGBA32F))
        {
            Log::error("[TerrainManipulator] setTerrainHeight: failed to convert image to RGBA32F.\n");
            return false;
        }
    }

    const ivec2 tileResolution = tile->getResolution();
    const bool useRegion = region.valid();
    const ivec2 drawCoord = useRegion ? region.coord : ivec2_zero;
    const ivec2 drawSize = useRegion ? region.size : tileResolution;
    if (preparedImage->getWidth() != drawSize.x || preparedImage->getHeight() != drawSize.y)
    {
        if (!preparedImage->resize(drawSize.x, drawSize.y))
        {
            Log::error("[TerrainManipulator] setTerrainHeight: failed to resize height image to %dx%d.\n", drawSize.x, drawSize.y);
            return false;
        }
    }

        // Use RAII - alphaImage is automatically cleaned up when going out of scope
    auto alphaImage = SurfaceRasterizer::createHeightAlphaImage(preparedImage);
    if (!alphaImage)
    {
        Log::error("[TerrainManipulator] setTerrainHeight: failed to create alpha image.\n");
        return false;
    }

    // Pre-create the overwrite material here so onTextureDraw does not need to
    // call loadInheritedMaterial per-tile inside the async callback.
    const MaterialPtr overwriteMaterial = loadInheritedMaterial("terrain_brush_r32f_overwrite.basebrush",
                                                                 "terrain height overwrite");
    if (!overwriteMaterial)
    {
        Log::error("[TerrainManipulator] setTerrainHeight: failed to load height overwrite material.\n");
        return false;
    }

    BrushOperationData operation;
    operation.brushMaterial = overwriteMaterial;
    operation.heightImage = preparedImage;
    operation.alphaImage = alphaImage;
    operation.modifyHeights = true;
    operation.drawCoord = drawCoord;
    operation.drawSize = drawSize;

    const int operationId = Landscape::generateOperationID();
    pendingOperations[operationId] = operation;
    Landscape::asyncTextureDraw(operationId,
                                tile->getGUID(),
                                drawCoord,
                                drawSize,
                                Landscape::FLAGS_FILE_DATA_HEIGHT | Landscape::FLAGS_FILE_DATA_OPACITY_HEIGHT);
    return true;
}

bool TerrainManipulator::setTerrainMask(const LandscapeLayerMapPtr& tile,
                                        const ImagePtr& maskImage,
                                        const TerrainBrushSettings& settings,
                                        int maskIndex,
                                        const SurfaceRasterizer::RasterRegion& region)
{
    if (!tile || !maskImage)
    {
        Log::error("[TerrainManipulator] setTerrainMask: null tile or mask image.\n");
        return false;
    }

    ImagePtr preparedImage = maskImage;
    if (preparedImage->getFormat() != Image::FORMAT_R8)
    {
        if (!preparedImage->convertToFormat(Image::FORMAT_R8))
        {
            Log::error("[TerrainManipulator] setTerrainMask: failed to convert image to R8.\n");
            return false;
        }
    }

    const ivec2 tileResolution = tile->getResolution();
    const bool useRegion = region.valid();
    const ivec2 drawCoord = useRegion ? region.coord : ivec2_zero;
    const ivec2 drawSize = useRegion ? region.size : tileResolution;
    if (preparedImage->getWidth() != drawSize.x || preparedImage->getHeight() != drawSize.y)
    {
        if (!preparedImage->resize(drawSize.x, drawSize.y))
        {
            Log::error("[TerrainManipulator] setTerrainMask: failed to resize mask image to %dx%d.\n", drawSize.x, drawSize.y);
            return false;
        }
    }

    const MaterialPtr brushMaterial = createMaskBrush(preparedImage);
    if (!brushMaterial)
    {
        Log::error("[TerrainManipulator] setTerrainMask: failed to create mask brush material.\n");
        return false;
    }

    BrushOperationData operation;
    operation.brushMaterial = brushMaterial;
    constexpr double kMinBrushSize = 1.0;
    operation.brushSize = static_cast<float>(std::max(kMinBrushSize, settings.brushSize));
    operation.modifyMask = true;
    operation.maskIndex = maskIndex;
    operation.drawCoord = drawCoord;
    operation.drawSize = drawSize;

    const int maskFlags = getMaskFileDataFlags(maskIndex);
    if (maskFlags == 0)
    {
        Log::error("[TerrainManipulator] setTerrainMask: invalid maskIndex %d — no file data flags.\n", maskIndex);
        return false;
    }

    const int operationId = Landscape::generateOperationID();
    pendingOperations[operationId] = operation;
    Landscape::asyncTextureDraw(operationId, tile->getGUID(), drawCoord, drawSize, maskFlags);
    return true;
}

bool TerrainManipulator::applyHeightOverwrite(const LandscapeTexturesPtr& buffer,
                                              const MaterialPtr& brushMaterial,
                                              const TexturePtr& heightTexture,
                                              const TexturePtr& alphaTexture)
{
    if (!buffer || !brushMaterial || !heightTexture || !alphaTexture)
    {
        Log::error("[TerrainManipulator] applyHeightOverwrite: null buffer, material, height texture, or alpha texture.\n");
        return false;
    }

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

bool TerrainManipulator::applyBrush(const BrushOperationData& operation,
                                    const LandscapeTexturesPtr& buffer,
                                    int dataMask)
{
    if (!buffer || !operation.brushMaterial)
    {
        Log::error("[TerrainManipulator] applyBrush: null buffer or brush material.\n");
        return false;
    }

    MaterialPtr brushMaterial = operation.brushMaterial;
    brushMaterial->setParameterFloat("size", operation.brushSize);
    brushMaterial->setParameterFloat("angle", operation.brushRotation);
    brushMaterial->setParameterFloat("opacity", operation.brushOpacity);

    if (operation.modifyMask)
        return applyMaskBrush(operation, buffer);

    if (operation.modifyHeights)
        return applyHeightBrushData(operation, buffer, dataMask);

    Log::error("[TerrainManipulator] applyBrush: no modify flag set on operation.\n");
    return false;
}

bool TerrainManipulator::applyHeightBrushData(const BrushOperationData& operation,
                                              const LandscapeTexturesPtr& buffer,
                                              int dataMask)
{
    MaterialPtr brushMaterial = operation.brushMaterial;
    brushMaterial->setTexture("terrain_height", buffer->getHeight());
    brushMaterial->setTexture("terrain_opacity_height", buffer->getOpacityHeight());
    brushMaterial->setParameterFloat("height", operation.brushHeight);
    brushMaterial->setParameterInt("data_mask", dataMask);
    brushMaterial->runExpression("brush", buffer->getResolution().x, buffer->getResolution().y);
    clearBrushMaterialTextures(brushMaterial);
    return true;
}

bool TerrainManipulator::applyMaskBrush(const BrushOperationData& operation,
                                        const LandscapeTexturesPtr& buffer)
{
    if (!buffer || !operation.brushMaterial)
    {
        Log::error("[TerrainManipulator] applyMaskBrush: null buffer or brush material.\n");
        return false;
    }

    MaterialPtr brushMaterial = operation.brushMaterial;
    int availablePages = 0;
    for (int pageIndex = 0; pageIndex < kLandscapeMaskPageCount; ++pageIndex)
    {
        if (buffer->getMask(pageIndex) && buffer->getOpacityMask(pageIndex))
            ++availablePages;
    }
    if (availablePages == 0)
    {
        Log::error("[TerrainManipulator] applyMaskBrush: no mask pages available in landscape buffer.\n");
        return false;
    }

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
            applied = applyHeightOverwrite(buffer, operation.brushMaterial, heightTexture, alphaTexture);
        }
    }
    else
    {
        applied = applyBrush(operation, buffer, dataMask);
    }

    if (applied)
        saveManager.markDirty(guid);

    endTransactionsIfIdle();
}

ImagePtr TerrainManipulator::createSolidHeightImage(const ivec2& resolution, float height)
{
    ImagePtr image = Image::create();
    image->create2D(resolution.x, resolution.y, Image::FORMAT_RGBA32F);

    Image::Pixel pixel;
    pixel.f.r = height;
    pixel.f.g = height;
    pixel.f.b = height;
    pixel.f.a = 1.0f;

    for (int x = 0; x < resolution.x; ++x)
    {
        for (int y = 0; y < resolution.y; ++y)
            image->set2D(x, y, pixel);
    }

    return image;
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

    // Each page holds 4 logical mask channels (RGBA). Only lock and write back
    // the single page that owns this maskIndex — not all pages.
    const int pageIndex = maskIndex / 4;
    const int flags = (Landscape::FLAGS_FILE_DATA_MASK_0 << pageIndex)
                    | (Landscape::FLAGS_FILE_DATA_OPACITY_MASK_0 << pageIndex);
    return flags;
}
