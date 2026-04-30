#include "SurfaceRasterizer.h"

#include "../core/NodeTreeWalker.h"

#include <UnigineLog.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Unigine;
using namespace Unigine::Math;

namespace
{
// Only surfaces whose world-space normal points mostly upward should drive
// terrain/mask rasterization. This excludes vertical side walls and underside
// triangles so pull/falloff stays under the intended top surface footprint.
constexpr double kMinTerrainSurfaceNormalZ = 0.7;

}

void SurfaceRasterizer::RasterBuffer::reset(const ivec2& resolutionValue)
{
    resolution = resolutionValue;
    const int pixelCount = resolution.x * resolution.y;
    values.assign(pixelCount, 0.0f);
    alpha.assign(pixelCount, 0.0f);
    sourceIndex.assign(pixelCount, -1);
    seeds.clear();
}

bool SurfaceRasterizer::buildSurfaceQuery(const std::string& pattern, SurfaceQuery& outQuery, std::string& errorMessage)
{
    outQuery.raw = pattern;
    outQuery.useRegex = false;

    if (pattern.empty())
    {
        errorMessage = "Surface name is empty.";
        return false;
    }

    // Only treat as regex when the pattern contains at least one regex metacharacter.
    // Plain names (e.g. "road_surface") are matched exactly via findSurface() without
    // compiling a regex, avoiding false regex interpretations of names like "mesh.001".
    static constexpr const char* kRegexMetaChars = R"(\.^$*+?{}[]|()\)";
    const bool hasMetaChars = pattern.find_first_of(kRegexMetaChars) != std::string::npos;
    if (!hasMetaChars)
        return true; // exact-match only, useRegex stays false

    try
    {
        outQuery.regex = std::regex(pattern);
        outQuery.useRegex = true;
    }
    catch (const std::regex_error& e)
    {
        errorMessage = std::string("Invalid regex pattern '") + pattern + "': " + e.what();
        return false;
    }

    return true;
}

std::vector<int> SurfaceRasterizer::findMatchingSurfaceIds(const ObjectMeshStaticPtr& mesh, const SurfaceQuery& query)
{
    std::vector<int> surfaceIds;
    if (!mesh)
        return surfaceIds;

    const int exactSurfaceId = mesh->findSurface(query.raw.c_str());
    if (exactSurfaceId >= 0)
    {
        surfaceIds.push_back(exactSurfaceId);
        return surfaceIds;
    }

    if (!query.useRegex)
        return surfaceIds;

    const int numSurfaces = mesh->getNumSurfaces();
    surfaceIds.reserve(numSurfaces);
    for (int surfaceIndex = 0; surfaceIndex < numSurfaces; ++surfaceIndex)
    {
        const std::string surfaceName = mesh->getSurfaceName(surfaceIndex);
        if (std::regex_search(surfaceName, query.regex))
            surfaceIds.push_back(surfaceIndex);
    }

    return surfaceIds;
}

std::vector<NodePtr> SurfaceRasterizer::collectMeshNodesRecursive(const std::vector<NodePtr>& roots)
{
    std::vector<NodePtr> collectedNodes;
    std::unordered_set<int> visitedNodeIds;
    std::unordered_set<int> collectedMeshIds;

    for (const auto& root : roots)
        NodeTreeWalker::collectMeshNodesRecursive(root, collectedNodes, visitedNodeIds, collectedMeshIds);

    return collectedNodes;
}

bool SurfaceRasterizer::rasterizeSurfaceHeight(const LandscapeLayerMapPtr& terrainTile,
                                               const ObjectSurface& objectSurface,
                                               RasterBuffer& outBuffer)
{
    if (!terrainTile)
        return false;

    outBuffer.reset(terrainTile->getResolution());

    Vector<dvec3> verticesWorldSpace;
    if (!appendSurfaceTrianglesWorldSpace(objectSurface, verticesWorldSpace))
        return false;

    const dmat4 inverseWorld = terrainTile->getIWorldTransform();
    const ivec2 resolution = terrainTile->getResolution();
    const Vec2 size = terrainTile->getSize();
    const Vec2 step = size / Vec2(resolution);
    const dvec2 stepD(static_cast<double>(step.x), static_cast<double>(step.y));
    bool modified = false;

    for (int vertexIndex = 2; vertexIndex < static_cast<int>(verticesWorldSpace.size()); vertexIndex += 3)
    {
        // Transform in DOUBLE precision first (world coords can be huge in UNIGINE_DOUBLE mode).
        // Casting dvec3 -> Vec3 BEFORE the inverse-world multiplication silently destroys
        // precision and sends the rasterized footprint to the wrong location.
        const dvec3 lv1D = inverseWorld * verticesWorldSpace[vertexIndex];
        const dvec3 lv2D = inverseWorld * verticesWorldSpace[vertexIndex - 1];
        const dvec3 lv3D = inverseWorld * verticesWorldSpace[vertexIndex - 2];

        // Down-cast to float only after we're in tile-local space (small magnitudes).
        const Vec3 localV1(static_cast<float>(lv1D.x), static_cast<float>(lv1D.y), static_cast<float>(lv1D.z));
        const Vec3 localV2(static_cast<float>(lv2D.x), static_cast<float>(lv2D.y), static_cast<float>(lv2D.z));
        const Vec3 localV3(static_cast<float>(lv3D.x), static_cast<float>(lv3D.y), static_cast<float>(lv3D.z));

        const Vec3 triV1(localV1.x, localV1.y, 0.0f);
        const Vec3 triV2(localV2.x, localV2.y, 0.0f);
        const Vec3 triV3(localV3.x, localV3.y, 0.0f);

        const int x1 = static_cast<int>(lv1D.x / stepD.x);
        const int y1 = static_cast<int>(lv1D.y / stepD.y);
        const int x2 = static_cast<int>(lv2D.x / stepD.x);
        const int y2 = static_cast<int>(lv2D.y / stepD.y);
        const int x3 = static_cast<int>(lv3D.x / stepD.x);
        const int y3 = static_cast<int>(lv3D.y / stepD.y);

        const int minX = std::max(0, std::min({x1, x2, x3}));
        const int minY = std::max(0, std::min({y1, y2, y3}));
        const int maxX = std::min(resolution.x - 1, std::max({x1, x2, x3}));
        const int maxY = std::min(resolution.y - 1, std::max({y1, y2, y3}));

        for (int x = minX; x <= maxX; ++x)
        {
            for (int y = minY; y <= maxY; ++y)
            {
                const int pixelIndex = toIndex(x, y, resolution.x);

                double baryU = 0.0;
                double baryV = 0.0;
                const Vec3 samplePoint((x + 0.5f) * step.x, (y + 0.5f) * step.y, 0.0f);
                if (!pointInTriangle(samplePoint, triV1, triV2, triV3, baryU, baryV))
                    continue;

                const Vec3 point = localV1 +
                    ((localV3 - localV1) * static_cast<float>(baryU)) +
                    ((localV2 - localV1) * static_cast<float>(baryV));

                // Convert tile-local height back to world space for terrain heightmap
                const dvec3 worldPointD = terrainTile->getWorldTransform() * dvec3(point.x, point.y, point.z);
                const float worldZ = static_cast<float>(worldPointD.z);

                // Keep the highest Z when multiple triangles cover the same pixel
                // (handles concave surfaces and overlapping geometry correctly).
                if (outBuffer.sourceIndex[pixelIndex] >= 0)
                {
                    if (worldZ > outBuffer.values[pixelIndex])
                        outBuffer.values[pixelIndex] = worldZ;
                    continue;
                }

                outBuffer.values[pixelIndex] = worldZ;
                outBuffer.alpha[pixelIndex] = 1.0f;
                outBuffer.sourceIndex[pixelIndex] = pixelIndex;
                outBuffer.seeds.push_back(pixelIndex);
                modified = true;
            }
        }
    }

    return modified;
}

bool SurfaceRasterizer::rasterizeSurfaceMask(const LandscapeLayerMapPtr& terrainTile,
                                             const ObjectSurface& objectSurface,
                                             RasterBuffer& outBuffer)
{
    if (!rasterizeSurfaceHeight(terrainTile, objectSurface, outBuffer))
        return false;

    for (int pixelIndex : outBuffer.seeds)
        outBuffer.values[pixelIndex] = 1.0f;

    return true;
}

bool SurfaceRasterizer::mergeRasterBuffer(RasterBuffer& target, const RasterBuffer& source)
{
    if (source.resolution.x <= 0 || source.resolution.y <= 0)
        return false;

    if (target.resolution != source.resolution)
        target.reset(source.resolution);

    bool modified = false;
    const int pixelCount = source.resolution.x * source.resolution.y;
    for (int pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex)
    {
        if (source.sourceIndex[pixelIndex] < 0 || source.alpha[pixelIndex] <= 0.0f)
            continue;

        if (target.sourceIndex[pixelIndex] < 0)
            target.seeds.push_back(pixelIndex);

        target.values[pixelIndex] = source.values[pixelIndex];
        target.alpha[pixelIndex] = source.alpha[pixelIndex];
        target.sourceIndex[pixelIndex] = pixelIndex;
        modified = true;
    }

    return modified;
}

void SurfaceRasterizer::applyDistanceFalloff(const LandscapeLayerMapPtr& terrainTile,
                                             RasterBuffer& buffer,
                                             double flatDistance,
                                             double falloffDistance)
{
    if (!terrainTile || buffer.empty())
        return;

    const ivec2 resolution = terrainTile->getResolution();
    if (resolution.x <= 0 || resolution.y <= 0)
        return;

    // Pixels-per-world-unit (use the larger axis so requested distances are at least respected).
    const Vec2 pixelsPerUnit = Vec2(resolution) / Vec2(terrainTile->getSize());
    const float ppu = std::max(pixelsPerUnit.x, pixelsPerUnit.y);
    const float flatPixels = static_cast<float>(std::max(0.0, flatDistance)) * ppu;
    const float falloffPixels = static_cast<float>(std::max(0.0, falloffDistance)) * ppu;
    const float totalPixels = flatPixels + falloffPixels;

    if (totalPixels <= 0.0f)
        return;

    // Chamfer 3-4 distance transform (two-pass) gives a near-Euclidean distance
    // field with smooth circular iso-lines, avoiding the diamond/octagon artifacts
    // of 4-connected BFS. We scale by 1/3 at the end so values are in pixel units.
    constexpr int kAxisCost = 3;     // cardinal neighbour
    constexpr int kDiagCost = 4;     // diagonal neighbour
    constexpr int kInfDistance = std::numeric_limits<int>::max() / 4;

    const int width = resolution.x;
    const int height = resolution.y;
    const int pixelCount = width * height;
    std::vector<int> dist(pixelCount, kInfDistance);
    std::vector<int> nearestSeed(pixelCount, -1);

    // Seed cells (rasterised triangle interior) have distance 0 and reference themselves.
    for (int seedIndex : buffer.seeds)
    {
        if (seedIndex >= 0 && seedIndex < pixelCount)
        {
            dist[seedIndex] = 0;
            nearestSeed[seedIndex] = seedIndex;
        }
    }

    auto relax = [&](int currentIndex, int neighbourIndex, int cost)
    {
        const int candidate = dist[currentIndex] + cost;
        if (candidate < dist[neighbourIndex])
        {
            dist[neighbourIndex] = candidate;
            nearestSeed[neighbourIndex] = nearestSeed[currentIndex];
        }
    };

    // Forward pass: top-left to bottom-right, look at NW/N/NE/W neighbours.
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const int idx = toIndex(x, y, width);
            if (y > 0)
            {
                if (x > 0)          relax(toIndex(x - 1, y - 1, width), idx, kDiagCost);
                                    relax(toIndex(x,     y - 1, width), idx, kAxisCost);
                if (x < width - 1)  relax(toIndex(x + 1, y - 1, width), idx, kDiagCost);
            }
            if (x > 0)              relax(toIndex(x - 1, y,     width), idx, kAxisCost);
        }
    }

    // Backward pass: bottom-right to top-left, look at SE/S/SW/E neighbours.
    for (int y = height - 1; y >= 0; --y)
    {
        for (int x = width - 1; x >= 0; --x)
        {
            const int idx = toIndex(x, y, width);
            if (y < height - 1)
            {
                if (x < width - 1)  relax(toIndex(x + 1, y + 1, width), idx, kDiagCost);
                                    relax(toIndex(x,     y + 1, width), idx, kAxisCost);
                if (x > 0)          relax(toIndex(x - 1, y + 1, width), idx, kDiagCost);
            }
            if (x < width - 1)      relax(toIndex(x + 1, y,     width), idx, kAxisCost);
        }
    }

    const auto smoothstep = [](float t) -> float
    {
        const float c = clamp(t, 0.0f, 1.0f);
        return (3.0f * c * c) - (2.0f * c * c * c);
    };

    // Convert chamfer distance to pixel-space Euclidean approximation (divide by axis cost)
    // and write height + alpha for every pixel within flat + falloff radius.
    for (int i = 0; i < pixelCount; ++i)
    {
        if (dist[i] >= kInfDistance)
            continue;

        const float pixelDist = static_cast<float>(dist[i]) / static_cast<float>(kAxisCost);
        if (pixelDist > totalPixels)
            continue;

        const int source = nearestSeed[i];
        if (source < 0)
            continue;

        if (buffer.sourceIndex[i] < 0)
        {
            buffer.sourceIndex[i] = source;
            buffer.values[i] = buffer.values[source];
        }

        if (pixelDist <= flatPixels || falloffPixels <= 0.0f)
        {
            buffer.alpha[i] = 1.0f;
        }
        else
        {
            const float t = (pixelDist - flatPixels) / falloffPixels;
            buffer.alpha[i] = smoothstep(1.0f - t);
        }
    }

}

void SurfaceRasterizer::blendFalloffWithExistingTerrain(const LandscapeLayerMapPtr& terrainTile,
                                                        const LandscapeFetchPtr& fetch,
                                                        RasterBuffer& buffer,
                                                        bool clampToOriginal)
{
    if (!terrainTile || !fetch)
        return;
    if (buffer.resolution.x <= 0 || buffer.resolution.y <= 0)
        return;

    const dmat4 tileWorld = terrainTile->getWorldTransform();
    const Vec2 tileSize = terrainTile->getSize();
    const ivec2 res = buffer.resolution;
    const Vec2 step = tileSize / Vec2(res);

    int falloffPixels = 0;
    int fetchFailures = 0;
    int clampedPixels = 0;

    for (int y = 0; y < res.y; ++y)
    {
        for (int x = 0; x < res.x; ++x)
        {
            const int i = toIndex(x, y, res.x);
            const float a = buffer.alpha[i];
            if (a <= 0.0f)
                continue; // pixel was not touched by the rasterizer/flood-fill

            if (a >= 1.0f)
            {
                // Flat zone: height is the pure mesh height.
                // If clamp is on and mesh would dig below terrain, keep original.
                if (clampToOriginal)
                {
                    const Vec3 lc((x + 0.5f) * step.x, (y + 0.5f) * step.y, 0.0f);
                    const dvec3 w = tileWorld * dvec3(lc.x, lc.y, 0.0);
                    const Vec2 fxy(static_cast<float>(w.x), static_cast<float>(w.y));
                    if (fetch->fetchForce(fxy))
                    {
                        const float existingH = fetch->getHeight();
                        if (buffer.values[i] < existingH)
                        {
                            buffer.values[i] = existingH;
                            ++clampedPixels;
                        }
                    }
                    else
                    {
                        buffer.alpha[i] = 0.0f; // no terrain data, skip pixel
                    }
                }
                continue;
            }

            // Falloff ring: blend mesh height with the existing terrain height
            // at this pixel's world-space XY. If the fetch succeeds we can
            // promote the pixel to full opacity with a blended height. If it
            // fails we leave the partial alpha so the brush still feathers
            // via the opacity channel rather than producing a mesh-height cliff.
            const Vec3 localCenter((x + 0.5f) * step.x, (y + 0.5f) * step.y, 0.0f);
            const dvec3 world = tileWorld * dvec3(localCenter.x, localCenter.y, 0.0);
            const Vec2 fetchXY(static_cast<float>(world.x), static_cast<float>(world.y));

            ++falloffPixels;
            if (fetch->fetchForce(fetchXY))
            {
                const float existingH = fetch->getHeight();
                const float meshH = buffer.values[i];
                float blendedH = existingH + (meshH - existingH) * a;

                // Clamp to prevent going below original terrain height (prevents edge dips)
                if (clampToOriginal && blendedH < existingH)
                {
                    blendedH = existingH;
                    ++clampedPixels;
                }

                buffer.values[i] = blendedH;
                buffer.alpha[i] = 1.0f;
            }
            else
            {
                // Fetch failed — no terrain data here (tile edge / outside terrain).
                // Zero out alpha so the brush does NOT paint this pixel at all.
                buffer.alpha[i] = 0.0f;
                ++fetchFailures;
            }
        }
    }

    if (falloffPixels > 0)
    {
        Unigine::Log::message("TerrainSurfaceTool: falloff blend -> %d pixels, %d fetch failures%s\n",
                              falloffPixels, fetchFailures,
                              clampedPixels > 0 ? std::string(", " + std::to_string(clampedPixels) + " clamped").c_str() : "");
    }
}

void SurfaceRasterizer::fillUnpaintedPixelsWithTerrain(const LandscapeLayerMapPtr& terrainTile,
                                                       const LandscapeFetchPtr& fetch,
                                                       RasterBuffer& buffer)
{
    if (!terrainTile || !fetch)
        return;
    if (buffer.resolution.x <= 0 || buffer.resolution.y <= 0)
        return;

    const dmat4 tileWorld = terrainTile->getWorldTransform();
    const Vec2 tileSize = terrainTile->getSize();
    const ivec2 res = buffer.resolution;
    const Vec2 step = tileSize / Vec2(res);

    for (int y = 0; y < res.y; ++y)
    {
        for (int x = 0; x < res.x; ++x)
        {
            const int i = toIndex(x, y, res.x);
            if (buffer.alpha[i] > 0.0f)
                continue; // already painted — leave it alone

            const Vec3 lc((x + 0.5f) * step.x, (y + 0.5f) * step.y, 0.0f);
            const dvec3 w = tileWorld * dvec3(lc.x, lc.y, 0.0);
            const Vec2 fxy(static_cast<float>(w.x), static_cast<float>(w.y));

            if (fetch->fetchForce(fxy))
                buffer.values[i] = fetch->getHeight();
            // alpha stays 0 — brush will write this height but with opacity=0
            // so Unigine won't actually change the stored terrain height
        }
    }
}

ImagePtr SurfaceRasterizer::createHeightImage(const RasterBuffer& buffer)
{
    if (buffer.resolution.x <= 0 || buffer.resolution.y <= 0)
        return nullptr;

    ImagePtr image = Image::create();
    image->create2D(buffer.resolution.x, buffer.resolution.y, Image::FORMAT_RGBA32F);

    for (int y = 0; y < buffer.resolution.y; ++y)
    {
        for (int x = 0; x < buffer.resolution.x; ++x)
        {
            const int index = toIndex(x, y, buffer.resolution.x);
            Image::Pixel pixel;
            pixel.f.r = buffer.values[index];
            pixel.f.g = buffer.values[index];
            pixel.f.b = buffer.values[index];
            pixel.f.a = buffer.alpha[index];
            image->set2D(x, y, pixel);
        }
    }

    return image;
}

ImagePtr SurfaceRasterizer::createHeightImage(const RasterBuffer& buffer, const RasterRegion& region)
{
    if (!region.valid())
        return nullptr;

    ImagePtr image = Image::create();
    image->create2D(region.size.x, region.size.y, Image::FORMAT_RGBA32F);

    for (int y = 0; y < region.size.y; ++y)
    {
        for (int x = 0; x < region.size.x; ++x)
        {
            const int sourceX = region.coord.x + x;
            const int sourceY = region.coord.y + y;
            const int index = toIndex(sourceX, sourceY, buffer.resolution.x);
            Image::Pixel pixel;
            pixel.f.r = buffer.values[index];
            pixel.f.g = buffer.values[index];
            pixel.f.b = buffer.values[index];
            pixel.f.a = buffer.alpha[index];
            image->set2D(x, y, pixel);
        }
    }

    return image;
}

ImagePtr SurfaceRasterizer::createHeightAlphaImage(const ImagePtr& heightImage)
{
    if (!heightImage)
        return nullptr;

    ImagePtr alphaImage = Image::create();
    alphaImage->create2D(heightImage->getWidth(), heightImage->getHeight(), Image::FORMAT_RGBA32F);

    for (int x = 0; x < heightImage->getWidth(); ++x)
    {
        for (int y = 0; y < heightImage->getHeight(); ++y)
        {
            const Image::Pixel src = heightImage->get2D(x, y);
            Image::Pixel dst;
            dst.f.r = src.f.a;
            dst.f.g = src.f.a;
            dst.f.b = src.f.a;
            dst.f.a = src.f.a;
            alphaImage->set2D(x, y, dst);
        }
    }

    return alphaImage;
}

ImagePtr SurfaceRasterizer::createHeightAlphaImage(const ImagePtr& heightImage, const RasterRegion& region)
{
    if (!heightImage || !region.valid())
        return nullptr;

    ImagePtr alphaImage = Image::create();
    alphaImage->create2D(region.size.x, region.size.y, Image::FORMAT_RGBA32F);

    for (int x = 0; x < region.size.x; ++x)
    {
        for (int y = 0; y < region.size.y; ++y)
        {
            const Image::Pixel src = heightImage->get2D(x, y);
            Image::Pixel dst;
            dst.f.r = src.f.a;
            dst.f.g = src.f.a;
            dst.f.b = src.f.a;
            dst.f.a = src.f.a;
            alphaImage->set2D(x, y, dst);
        }
    }

    return alphaImage;
}

ImagePtr SurfaceRasterizer::createMaskImage(const RasterBuffer& buffer)
{
    if (buffer.resolution.x <= 0 || buffer.resolution.y <= 0)
        return nullptr;

    ImagePtr image = Image::create();
    image->create2D(buffer.resolution.x, buffer.resolution.y, Image::FORMAT_R8);

    for (int y = 0; y < buffer.resolution.y; ++y)
    {
        for (int x = 0; x < buffer.resolution.x; ++x)
        {
            const int index = toIndex(x, y, buffer.resolution.x);
            Image::Pixel pixel;
            pixel.i.r = clamp(static_cast<int>(buffer.alpha[index] * 255.0f), 0, 255);
            image->set2D(x, y, pixel);
        }
    }

    return image;
}

SurfaceRasterizer::RasterRegion SurfaceRasterizer::calculateTouchedRegion(const RasterBuffer& buffer)
{
    RasterRegion region;
    if (buffer.resolution.x <= 0 || buffer.resolution.y <= 0)
        return region;

    int minX = buffer.resolution.x;
    int minY = buffer.resolution.y;
    int maxX = -1;
    int maxY = -1;

    for (int y = 0; y < buffer.resolution.y; ++y)
    {
        for (int x = 0; x < buffer.resolution.x; ++x)
        {
            const int index = toIndex(x, y, buffer.resolution.x);
            if (buffer.alpha[index] <= 0.0f)
                continue;

            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }

    if (maxX < minX || maxY < minY)
        return region;

    region.coord = ivec2(minX, minY);
    region.size = ivec2(maxX - minX + 1, maxY - minY + 1);
    return region;
}

ImagePtr SurfaceRasterizer::createMaskImage(const RasterBuffer& buffer, const RasterRegion& region)
{
    if (!region.valid())
        return nullptr;

    ImagePtr image = Image::create();
    image->create2D(region.size.x, region.size.y, Image::FORMAT_R8);

    for (int y = 0; y < region.size.y; ++y)
    {
        for (int x = 0; x < region.size.x; ++x)
        {
            const int sourceX = region.coord.x + x;
            const int sourceY = region.coord.y + y;
            const int index = toIndex(sourceX, sourceY, buffer.resolution.x);
            Image::Pixel pixel;
            pixel.i.r = clamp(static_cast<int>(buffer.alpha[index] * 255.0f), 0, 255);
            image->set2D(x, y, pixel);
        }
    }

    return image;
}

int SurfaceRasterizer::toIndex(int x, int y, int width)
{
    return x + (width * y);
}

bool SurfaceRasterizer::pointInTriangle(const Vec3& point,
                                        const Vec3& v1,
                                        const Vec3& v2,
                                        const Vec3& v3,
                                        double& outU,
                                        double& outV)
{
    const Vec3 v0 = v3 - v1;
    const Vec3 e1 = v2 - v1;
    const Vec3 e2 = point - v1;

    const double dot00 = dot(v0, v0);
    const double dot01 = dot(v0, e1);
    const double dot02 = dot(v0, e2);
    const double dot11 = dot(e1, e1);
    const double dot12 = dot(e1, e2);

    const double denominator = (dot00 * dot11) - (dot01 * dot01);
    if (std::abs(denominator) <= kEpsilon)
        return false;

    const double inverseDenominator = 1.0 / denominator;
    outU = ((dot11 * dot02) - (dot01 * dot12)) * inverseDenominator;
    outV = ((dot00 * dot12) - (dot01 * dot02)) * inverseDenominator;
    return (outU >= 0.0 && outV >= 0.0 && (outU + outV) <= 1.0);
}

bool SurfaceRasterizer::isUpFacingTriangle(const dvec3& v1, const dvec3& v2, const dvec3& v3)
{
    const dvec3 edge1 = v2 - v1;
    const dvec3 edge2 = v3 - v1;
    const dvec3 normal = cross(edge1, edge2);
    const double length = normal.length();
    if (length <= Consts::EPS_D)
        return false;

    // Accept only upward-facing triangles (positive Z normal).
    const double normalizedZ = normal.z / length;
    return normalizedZ >= kMinTerrainSurfaceNormalZ;
}

bool SurfaceRasterizer::appendSurfaceTrianglesWorldSpace(const ObjectSurface& objectSurface,
                                                         Vector<dvec3>& outVertices)
{
    outVertices.clear();

    if (!objectSurface.first)
        return false;

    const dmat4 worldTransform = objectSurface.first->getWorldTransform();
    const auto object = objectSurface.first;
    const int surfaceIndex = objectSurface.second;

    if (object->getType() != Node::OBJECT_MESH_STATIC)
        return false;

    ObjectMeshStaticPtr meshStaticObject = dynamic_ptr_cast<ObjectMeshStatic>(object);
    if (!meshStaticObject)
        return false;

    MeshStaticPtr mesh = meshStaticObject->getMeshForce();
    if (!mesh)
        return false;

    const Vector<int>& indices = mesh->getCIndices(surfaceIndex);
    const int surfaceVertexCount = mesh->getNumVertices(surfaceIndex);

    outVertices.reserve(indices.size());
    for (int indexOffset = 0; indexOffset + 2 < indices.size(); indexOffset += 3)
    {
        const int index1 = indices[indexOffset];
        const int index2 = indices[indexOffset + 1];
        const int index3 = indices[indexOffset + 2];
        if (index1 < 0 || index1 >= surfaceVertexCount ||
            index2 < 0 || index2 >= surfaceVertexCount ||
            index3 < 0 || index3 >= surfaceVertexCount)
            continue;

        const dvec3 vertex1 = worldTransform * dvec3(mesh->getVertex(index1, surfaceIndex));
        const dvec3 vertex2 = worldTransform * dvec3(mesh->getVertex(index2, surfaceIndex));
        const dvec3 vertex3 = worldTransform * dvec3(mesh->getVertex(index3, surfaceIndex));
        if (!isUpFacingTriangle(vertex1, vertex2, vertex3))
            continue;

        outVertices.push_back(vertex1);
        outVertices.push_back(vertex2);
        outVertices.push_back(vertex3);
    }

    return !outVertices.empty();
}
