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

class TerrainManipulator
{
public:
    using LogFn = std::function<void(const std::string&)>;

    explicit TerrainManipulator(LandscapeSaveManager& saveManager);
    ~TerrainManipulator();

    bool pullTerrainToSurface(const std::vector<Unigine::NodePtr>& nodes,
                              const Unigine::ObjectLandscapeTerrainPtr& terrain,
                              const Unigine::LandscapeLayerMapPtr& targetTile,
                              const std::string& surfacePattern,
                              const TerrainBrushSettings& settings,
                              const LogFn& log);

    bool applyLandscapeMask(const std::vector<Unigine::NodePtr>& nodes,
                            const Unigine::ObjectLandscapeTerrainPtr& terrain,
                            const Unigine::LandscapeLayerMapPtr& targetTile,
                            const std::string& surfacePattern,
                            const TerrainBrushSettings& settings,
                            int maskIndex,
                            const LogFn& log);

    bool resetTerrainHeights(const std::vector<Unigine::NodePtr>& nodes,
                             const Unigine::ObjectLandscapeTerrainPtr& terrain,
                             const Unigine::LandscapeLayerMapPtr& targetTile,
                             const LogFn& log);

    bool paintWhiteHeight(const std::vector<Unigine::NodePtr>& nodes,
                          const Unigine::ObjectLandscapeTerrainPtr& terrain,
                          const Unigine::LandscapeLayerMapPtr& targetTile,
                          const TerrainBrushSettings& settings,
                          const LogFn& log);

    bool isBusy() const;
    size_t pendingOperationCount() const;
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

    static constexpr bool kDebugHotPathLogs = false;
    static constexpr bool kSaveDebugRasterImages = false;
    static constexpr const char* kDebugRasterOutputDir = "C:/Temp";
    static constexpr int kLandscapeMaskPageCount = 5;
    static constexpr int kMaxLandscapeMaskIndex = 19;
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
    static void saveDebugRasterImages(const Unigine::LandscapeLayerMapPtr& tile,
                                      const SurfaceRasterizer::RasterBuffer& rasterBuffer,
                                      const LogFn& log);

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
