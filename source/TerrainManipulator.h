#pragma once

#include "LandscapeSaveManager.h"
#include "SurfaceRasterizer.h"

#include <UnigineEvent.h>
#include <UnigineImage.h>
#include <UnigineMaterials.h>
#include <UnigineMeshStatic.h>
#include <UnigineObjects.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct TerrainBrushSettings
{
    double brush_size = 10.0;
    double flat_distance = 30.0;
    double falloff_distance = 30.0;
    double smoothing_strength = 0.5;
};

class TerrainManipulator
{
public:
    using LogFn = std::function<void(const std::string&)>;

    explicit TerrainManipulator(LandscapeSaveManager& save_manager);
    ~TerrainManipulator();

    bool pullTerrainToSurface(const std::vector<Unigine::NodePtr>& nodes,
                              const std::string& surface_pattern,
                              const TerrainBrushSettings& settings,
                              const LogFn& log);

    bool applyLandscapeMask(const std::vector<Unigine::NodePtr>& nodes,
                            const std::string& surface_pattern,
                            const TerrainBrushSettings& settings,
                            int mask_index,
                            const LogFn& log);

    bool resetTerrainHeights(const std::vector<Unigine::NodePtr>& nodes,
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
        std::vector<Unigine::LandscapeLayerMapPtr> layer_maps;
        Unigine::LandscapeFetchPtr fetch;
    };

    struct BrushOperationData
    {
        Unigine::MaterialPtr brush_material;
        Unigine::ImagePtr albedo_image;
        Unigine::ImagePtr height_image;
        Unigine::ImagePtr alpha_image;
        float brush_height = 0.0f;
        float brush_size = 1.0f;
        float brush_rotation = 0.0f;
        float brush_opacity = 1.0f;
        HeightBlendMode height_blend_mode = HeightBlendMode::Alpha;
        bool modify_heights = false;
        bool modify_albedo = false;
        bool modify_mask = false;
        int mask_index = 0;
    };

    static constexpr bool kDebugHotPathLogs = false;

    bool beginActionTransaction();
    void finishActionScheduling();
    void finalizeActionTransactionsIfIdle();

    static TerrainContext buildTerrainContext();
    static bool terrainAvailable(TerrainContext& context, const Unigine::Math::dvec2& position);
    static Unigine::LandscapeLayerMapPtr findContainingLayerMap(const TerrainContext& context,
                                                                const Unigine::Math::dvec3& position);

    void raiseTerrainAtPoint(const TerrainContext& context,
                             const Unigine::Math::dvec3& point,
                             const Unigine::Math::dvec2& brush_size,
                             const Unigine::MaterialPtr& brush_material,
                             float brush_rotation,
                             bool target_albedo = false,
                             Unigine::LandscapeLayerMapPtr forced_tile = nullptr);

    static void calculateDrawingRegion(const Unigine::Math::dvec3& brush_local_position,
                                       const Unigine::Math::quat& brush_local_rotation,
                                       const Unigine::Math::dvec2& half_size,
                                       const Unigine::LandscapeLayerMapPtr& tile,
                                       Unigine::Math::ivec2& out_coord,
                                       Unigine::Math::ivec2& out_size);

    bool setTerrainHeight(const Unigine::LandscapeLayerMapPtr& tile, const Unigine::ImagePtr& height_image);
    bool setTerrainMask(const Unigine::LandscapeLayerMapPtr& tile,
                        const Unigine::ImagePtr& mask_image,
                        const TerrainBrushSettings& settings,
                        int mask_index);
    bool applyHeightOverwrite(const Unigine::LandscapeTexturesPtr& buffer,
                              const Unigine::TexturePtr& height_texture,
                              const Unigine::TexturePtr& alpha_texture);
    bool applyAlbedoOverwrite(const Unigine::LandscapeTexturesPtr& buffer,
                              const Unigine::ImagePtr& albedo_image);
    bool applyBrush(const BrushOperationData& operation,
                    const Unigine::LandscapeTexturesPtr& buffer,
                    int data_mask);

    void onTextureDraw(const Unigine::UGUID& guid, int operation_id,
                       const Unigine::LandscapeTexturesPtr& buffer,
                       const Unigine::Math::ivec2& coord,
                       int data_mask);

    static Unigine::MaterialPtr loadInheritedMaterial(const char* material_path, const char* log_context);
    static Unigine::MaterialPtr createCircularBrush(double falloff_ratio, double padding = 0.0);
    static Unigine::MaterialPtr createMaskBrush(const Unigine::ImagePtr& mask_image);
    static int getMaskFileDataFlags(int mask_index);

    LandscapeSaveManager& save_manager_;
    std::unordered_map<int, BrushOperationData> pending_operations_;

    Unigine::EventConnectionId texture_draw_connection_id_ = nullptr;
    int pending_transaction_commits_ = 0;
    bool in_progress_ = false;
};
