#include "DebugImageExporter.h"

#include <UnigineLog.h>

#include <algorithm>
#include <string>

using namespace Unigine;
using namespace Unigine::Math;

bool DebugImageExporter::enabled = false;

void DebugImageExporter::logMessage(const LogFn& log, const std::string& message)
{
    if (log)
        log(message);
}

void DebugImageExporter::save(const LandscapeLayerMapPtr& tile,
                              const SurfaceRasterizer::RasterBuffer& rasterBuffer,
                              const LogFn& log)
{
    if (!enabled || !tile)
        return;

    const std::string tileName = std::string(tile->getName());
    const std::string heightPath = std::string(kOutputDir) + "/debug_height_" + tileName + ".png";
    const std::string alphaPath = std::string(kOutputDir) + "/debug_alpha_" + tileName + ".png";
    const ivec2 resolution = rasterBuffer.resolution;

    // Find the non-zero height range for normalization.
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
