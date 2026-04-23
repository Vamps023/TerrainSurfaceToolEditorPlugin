#pragma once

struct TerrainBrushSettings
{
    double brush_size = 10.0;
    double flat_distance = 30.0;
    double falloff_distance = 30.0;
    double smoothing_strength = 0.5;
    bool clamp_to_original = false;
};
