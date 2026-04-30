#pragma once

// Highest valid logical landscape mask index (5 pages x 4 RGBA channels - 1).
// Referenced by TerrainManipulator and TerrainToolPanel.
static constexpr int kMaxLandscapeMaskIndex = 19;

// Settings that control how the terrain brush rasterizes and blends
// the mesh surface footprint onto the landscape heightmap/mask.
struct TerrainBrushSettings
{
    // Legacy stamp-mode brush radius (world units). Only used by the mask
    // apply path; the pull-terrain path uses flat/falloff distances instead.
    double brushSize = 10.0;

    // Padding (world units) around the mesh footprint that is kept at the
    // exact mesh height with full opacity (alpha = 1).
    double flatDistance = 30.0;

    // Width (world units) of the smooth blend zone beyond the flat region
    // where the terrain transitions back to its original height.
    double falloffDistance = 30.0;

    // Reserved for future smoothing pass strength [0..1].
    double smoothingStrength = 0.5;

    // When true, blended heights in the falloff zone are clamped so they
    // never go below the original terrain height (prevents edge dips).
    bool clampToOriginal = false;
};
