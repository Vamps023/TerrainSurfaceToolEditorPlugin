#pragma once

#include "../rasterizer/SurfaceRasterizer.h"

#include <UnigineImage.h>
#include <UnigineObjects.h>

#include <functional>
#include <string>

// Exports debug preview images (height/alpha PNGs) from a RasterBuffer.
// Controlled by compile-time and runtime flags. All methods are static.
class DebugImageExporter
{
public:
    using LogFn = std::function<void(const std::string&)>;

    // Set to true to enable debug image export at runtime.
    static bool enabled;

    // Directory where debug images are written. Must exist and be writable.
    static constexpr const char* kOutputDir = "C:/Temp";

    // Saves height and alpha preview PNGs for the given tile and raster buffer.
    // Height is normalized to the [min..max] range of non-zero values for visibility.
    // Alpha is written as a grayscale image.
    static void save(const Unigine::LandscapeLayerMapPtr& tile,
                     const SurfaceRasterizer::RasterBuffer& rasterBuffer,
                     const LogFn& log);

private:
    static void logMessage(const LogFn& log, const std::string& message);
};
