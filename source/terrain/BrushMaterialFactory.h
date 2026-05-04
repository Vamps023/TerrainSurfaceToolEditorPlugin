#pragma once

#include <UnigineImage.h>
#include <UnigineMaterials.h>

// Factory for creating and configuring Unigine brush materials used by
// Landscape::asyncTextureDraw. All methods are static — this is a pure utility class.
class BrushMaterialFactory
{
public:
    // Loads a base material by virtual path and returns an inherited instance.
    // Returns nullptr if the material file cannot be found or loaded.
    [[nodiscard]] static Unigine::MaterialPtr loadInheritedMaterial(const char* materialPath, const char* logContext);

    // Creates a mask brush material from the given mask image (R8 format).
    // The image is bound to the "opacity" texture slot.
    [[nodiscard]] static Unigine::MaterialPtr createMaskBrush(const Unigine::ImagePtr& maskImage);

    // Clears all terrain source textures from a brush material to prevent
    // them from leaking into subsequent operations that reuse the material.
    static void clearTerrainTextures(const Unigine::MaterialPtr& brushMaterial);

    // Clears mask page textures (terrain_mask_N / terrain_opacity_mask_N) from
    // the brush material. Iterates all 5 mask pages.
    static void clearMaskTextures(const Unigine::MaterialPtr& brushMaterial);

private:
    // Number of landscape mask texture pages (each page holds 4 RGBA channels).
    static constexpr int kLandscapeMaskPageCount = 5;
};
