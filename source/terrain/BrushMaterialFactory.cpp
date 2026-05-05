#include "BrushMaterialFactory.h"

#include <UnigineFileSystem.h>
#include <UnigineLog.h>

#include <string>

using namespace Unigine;
using namespace Unigine::Math;

MaterialPtr BrushMaterialFactory::loadInheritedMaterial(const char* materialPath, const char* logContext)
{
    const auto fileGuid = FileSystem::getGUID(FileSystem::resolvePartialVirtualPath(materialPath));
    if (!fileGuid.isValid())
    {
        Log::error("[BrushMaterialFactory] Missing %s material '%s'\n", logContext, materialPath);
        return nullptr;
    }

    const auto baseMaterial = Materials::findMaterialByFileGUID(fileGuid);
    if (!baseMaterial)
    {
        Log::error("[BrushMaterialFactory] Failed to load %s material '%s'\n", logContext, materialPath);
        return nullptr;
    }

    return baseMaterial->inherit();
}

MaterialPtr BrushMaterialFactory::createMaskBrush(const ImagePtr& maskImage)
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

void BrushMaterialFactory::clearTerrainTextures(const MaterialPtr& brushMaterial)
{
    if (!brushMaterial)
        return;

    brushMaterial->setTexture("terrain_height", nullptr);
    brushMaterial->setTexture("terrain_opacity_height", nullptr);
    brushMaterial->setTexture("terrain_albedo", nullptr);
}

void BrushMaterialFactory::clearMaskTextures(const MaterialPtr& brushMaterial)
{
    if (!brushMaterial)
        return;

    for (int pageIndex = 0; pageIndex < kLandscapeMaskPageCount; ++pageIndex)
    {
        const std::string maskName = "terrain_mask_" + std::to_string(pageIndex);
        const std::string opacityName = "terrain_opacity_mask_" + std::to_string(pageIndex);
        brushMaterial->setTexture(maskName.c_str(), nullptr);
        brushMaterial->setTexture(opacityName.c_str(), nullptr);
    }
}
