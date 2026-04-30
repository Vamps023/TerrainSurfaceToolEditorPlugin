#pragma once

#include "TerrainBrushSettings.h"
#include "../landscape/LandscapeSaveManager.h"
#include "../rasterizer/SurfaceRasterizer.h"

#include <UnigineEvent.h>
#include <UnigineImage.h>
#include <UnigineMaterials.h>
#include <UnigineMesh.h>
#include <UnigineObjects.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Orchestrates the full pipeline from mesh-surface rasterization through
// CPU falloff/blend to Landscape::asyncTextureDraw brush dispatch.
// All heavy operations are asynchronous; use isBusy() to poll completion.
class TerrainManipulator
{
public:
    using LogFn = std::function<void(const std::string&)>;

    // Constructs a TerrainManipulator instance, associating it with the given LandscapeSaveManager.
    explicit TerrainManipulator(LandscapeSaveManager& saveManager);
    ~TerrainManipulator();

    // Rasterizes mesh surfaces matching surfacePattern onto the terrain heightmap.
    // Applies flat-distance padding and falloff blending before writing.
    // Returns true if at least one tile operation was queued.
    bool pullTerrainToSurface(const std::vector<Unigine::NodePtr>& nodes,
                              const Unigine::ObjectLandscapeTerrainPtr& terrain,
                              const Unigine::LandscapeLayerMapPtr& targetTile,
                              const std::string& surfacePattern,
                              const TerrainBrushSettings& settings,
                              const LogFn& log);

    // Rasterizes mesh surface footprint into the landscape mask channel maskIndex [0..19].
    // Returns true if at least one tile mask operation was queued.
    bool applyLandscapeMask(const std::vector<Unigine::NodePtr>& nodes,
                            const Unigine::ObjectLandscapeTerrainPtr& terrain,
                            const Unigine::LandscapeLayerMapPtr& targetTile,
                            const std::string& surfacePattern,
                            const TerrainBrushSettings& settings,
                            int maskIndex,
                            const LogFn& log);

    // Resets the heightmap to 0 for all tiles that intersect the given nodes.
    // Returns true if at least one tile was modified.
    bool resetTerrainHeights(const std::vector<Unigine::NodePtr>& nodes,
                             const Unigine::ObjectLandscapeTerrainPtr& terrain,
                             const Unigine::LandscapeLayerMapPtr& targetTile,
                             const LogFn& log);

    // Erases all heightmap data (sets height to 0) on all tiles of the terrain.
    // Returns true if at least one tile was modified.
    bool paintWhiteHeight(const std::vector<Unigine::NodePtr>& nodes,
                          const Unigine::ObjectLandscapeTerrainPtr& terrain,
                          const Unigine::LandscapeLayerMapPtr& targetTile,
                          const TerrainBrushSettings& settings,
                          const LogFn& log);

    // Returns true while async texture-draw operations are still in flight.
    bool isBusy() const;
    // Returns the number of queued but not yet dispatched brush operations.
    size_t pendingOperationCount() const;
    // Forces an immediate flush of all pending landscape saves.
    void flushPendingSaves();

private:
    enum class HeightBlendMode
    {
        Alpha = 0,
        Additive = 1,
    };

    struct TerrainContext
    {
        Unigine::ObjectLandscapeTerrainPtr terrain;
        std::vector<Unigine::LandscapeLayerMapPtr> layerMaps;
        Unigine::LandscapeFetchPtr fetch;
    };

    struct BrushOperationData
    {
        Unigine::MaterialPtr brushMaterial;
        Unigine::ImagePtr albedoImage;
        Unigine::ImagePtr heightImage;
        Unigine::ImagePtr alphaImage;
        float brushHeight = 0.0f;
        float brushSize = 1.0f;
        float brushRotation = 0.0f;
        float brushOpacity = 1.0f;
        HeightBlendMode heightBlendMode = HeightBlendMode::Alpha;
        bool modifyHeights = false;
        bool modifyAlbedo = false;
        bool modifyMask = false;
        int maskIndex = 0;
        Unigine::Math::ivec2 drawCoord = Unigine::Math::ivec2_zero;
        Unigine::Math::ivec2 drawSize = Unigine::Math::ivec2_zero;
    };

    // Set to true to enable verbose per-frame logging on hot paths.
    static constexpr bool kDebugHotPathLogs = false;
    // Set to true to save debug height/alpha PNG images to kDebugRasterOutputDir.
    static constexpr bool kSaveDebugRasterImages = false;
    // Directory used when kSaveDebugRasterImages is true.
    static constexpr const char* kDebugRasterOutputDir = "C:/Temp";
    // Number of landscape mask texture pages (each page holds 4 RGBA channels).
    static constexpr int kLandscapeMaskPageCount = 5;
    // Highest valid logical mask index (5 pages x 4 channels - 1 = 19).
    static constexpr int kMaxLandscapeMaskIndex = 19;
    // Unigine reserves the first 2 bits of the data-mask word for height data
    // (bit 0 = height, bit 1 = opacity-height). Landscape mask channels start at bit 2.
    static constexpr int kMaskDataBitOffset = 2;

    bool beginActionTransaction();
    void finishActionScheduling();
    void EndTransactionsIfIdle();

    static TerrainContext buildTerrainContext(const Unigine::ObjectLandscapeTerrainPtr& terrain,
                                             const Unigine::LandscapeLayerMapPtr& targetTile);
    static bool terrainAvailable(TerrainContext& context, const Unigine::Math::dvec2& position);
    static Unigine::LandscapeLayerMapPtr findContainingLayerMap(const TerrainContext& context,
                                                                const Unigine::Math::dvec3& position);

    void raiseTerrainAtPoint(const TerrainContext& context,
                             const Unigine::Math::dvec3& point,
                             const Unigine::Math::dvec2& brushSize,
                             const Unigine::MaterialPtr& brushMaterial,
                             float brushRotation,
                             bool targetAlbedo = false,
                             Unigine::LandscapeLayerMapPtr forcedTile = nullptr);

    static void calculateDrawingRegion(const Unigine::Math::dvec3& brushLocalPosition,
                                       const Unigine::Math::quat& brushLocalRotation,
                                       const Unigine::Math::dvec2& halfSize,
                                       const Unigine::LandscapeLayerMapPtr& tile,
                                       Unigine::Math::ivec2& outCoord,
                                       Unigine::Math::ivec2& outSize);

    bool setTerrainHeight(const Unigine::LandscapeLayerMapPtr& tile,
                          const Unigine::ImagePtr& heightImage,
                          const SurfaceRasterizer::RasterRegion& region = SurfaceRasterizer::RasterRegion());
    bool setTerrainMask(const Unigine::LandscapeLayerMapPtr& tile,
                        const Unigine::ImagePtr& maskImage,
                        const TerrainBrushSettings& settings,
                        int maskIndex,
                        const SurfaceRasterizer::RasterRegion& region = SurfaceRasterizer::RasterRegion());
    bool applyHeightOverwrite(const Unigine::LandscapeTexturesPtr& buffer,
                              const Unigine::TexturePtr& heightTexture,
                              const Unigine::TexturePtr& alphaTexture);
    bool applyAlbedoOverwrite(const Unigine::LandscapeTexturesPtr& buffer,
                              const Unigine::ImagePtr& albedoImage);
    bool applyBrush(const BrushOperationData& operation,
                    const Unigine::LandscapeTexturesPtr& buffer,
                    int dataMask);
    bool applyMaskBrush(const BrushOperationData& operation,
                        const Unigine::LandscapeTexturesPtr& buffer);
    bool queueHeightRasterForTile(const TerrainContext& terrainContext,
                                  const Unigine::LandscapeLayerMapPtr& tile,
                                  SurfaceRasterizer::RasterBuffer& rasterBuffer,
                                  double flatDistance,
                                  double falloffDistance,
                                  const TerrainBrushSettings& settings,
                                  const LogFn& log);
    // Applies falloff + mask image creation for a single tile in applyLandscapeMask.
    bool queueMaskRasterForTile(const Unigine::LandscapeLayerMapPtr& tile,
                                SurfaceRasterizer::RasterBuffer& rasterBuffer,
                                const TerrainBrushSettings& settings,
                                int maskIndex,
                                const LogFn& log);
    static void saveDebugRasterImages(const Unigine::LandscapeLayerMapPtr& tile,
                                      const SurfaceRasterizer::RasterBuffer& rasterBuffer,
                                      const LogFn& log);
    // Sub-methods used by applyBrush to handle each modification mode.
    bool applyHeightBrushData(const BrushOperationData& operation,
                              const Unigine::LandscapeTexturesPtr& buffer,
                              int dataMask);
    bool applyAlbedoBrushData(const BrushOperationData& operation,
                              const Unigine::LandscapeTexturesPtr& buffer,
                              int dataMask);

    void onTextureDraw(const Unigine::UGUID& guid, int operationId,
                       const Unigine::LandscapeTexturesPtr& buffer,
                       const Unigine::Math::ivec2& coord,
                       int dataMask);

    static Unigine::MaterialPtr loadInheritedMaterial(const char* materialPath, const char* logContext);
    static void clearBrushMaterialTextures(const Unigine::MaterialPtr& brushMaterial);
    static Unigine::MaterialPtr createCircularBrush(double falloffRatio, double padding = 0.0);
    static Unigine::MaterialPtr createMaskBrush(const Unigine::ImagePtr& maskImage);
    static int getMaskFileDataFlags(int maskIndex);

    LandscapeSaveManager& saveManager;
    std::unordered_map<int, BrushOperationData> pendingOperations;

    Unigine::EventConnection textureDrawConnection;
    int pendingTransactionCommits = 0;
    bool inProgress = false;
};
