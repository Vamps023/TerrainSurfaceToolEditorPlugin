#pragma once

struct TerrainBrushSettings
{
    double brushSize = 10.0;
    double flatDistance = 30.0;
    double falloffDistance = 30.0;
    double smoothingStrength = 0.5;
    bool clampToOriginal = false;
};
