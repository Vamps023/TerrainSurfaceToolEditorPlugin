#pragma once

#include <regex>
#include <string>

// Compiled surface-name filter: supports exact name matching and regex.
// Kept in its own header so <regex> is only pulled in by translation units
// that actually process surface queries, not every includer of SurfaceRasterizer.h.
struct SurfaceQuery
{
    std::string raw;       // original pattern string
    bool useRegex = false; // true when pattern compiled as regex
    std::regex regex;      // compiled regex (valid only when useRegex == true)
};
