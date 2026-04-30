# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] — 2025-04-30

### Added

- First release of the consolidated terrain editor plugin for Unigine 2
- Pull Terrain To Surface: rasterizes mesh surfaces onto the landscape heightmap with flat-distance padding and smooth falloff blending
- Apply to Landscape Mask: rasterizes mesh surface footprints into any of the 20 logical landscape mask channels
- Paint Complete White Height: erases heightmap data to 0 m on a selected tile or all tiles
- Deferred save manager with transaction batching to prevent redundant async saves
- Region-cropped asyncTextureDraw dispatch to minimise GPU upload area


