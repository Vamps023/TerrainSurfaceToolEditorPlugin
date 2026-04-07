#pragma once

/* ========================================================================
 * TerrainSurfaceToolEditorPlugin.h
 * 
 * Single Unigine 2.18 Editor Plugin that implements "Pull Terrain To Surface".
 * Refactored from terrain_editor2_plugin + terrain_editor2_editor_plugin.
 * All terrain manipulation logic is integrated directly.
 * ======================================================================== */

#include <editor/UniginePlugin.h>
#include <editor/UnigineConstants.h>
#include <editor/UnigineWindowManager.h>
#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>

#include <UnigineLog.h>
#include <UnigineEditor.h>
#include <UnigineNode.h>
#include <UnigineNodes.h>
#include <UnigineObjects.h>
#include <UnigineMathLib.h>
#include <UnigineImage.h>
#include <UnigineMaterials.h>
#include <UnigineFileSystem.h>
#include <UnigineWorld.h>
#include <UnigineWorlds.h>
#include <UnigineEvent.h>

#include <QObject>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>
#include <QPushButton>
#include <QProgressBar>
#include <QTextEdit>
#include <QGroupBox>
#include <QMenu>
#include <QAction>
#include <QMenuBar>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QScrollBar>
#include <UnigineMeshStatic.h>

#include <queue>
#include <vector>
#include <string>
#include <regex>
#include <map>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <functional>
#include <sstream>

/* ========================================================================
 * ObjectSurface type alias (from ObjectMeshHelper.h)
 * ======================================================================== */
using ObjectSurface = std::pair<Unigine::ObjectPtr, int>;

/* ========================================================================
 * TerrainManipulator - Core terrain modification class
 * Refactored from terrain_editor2_plugin/TerrainManipulator.h/.cpp
 * ======================================================================== */
class TerrainManipulator
{
public:
    TerrainManipulator();
    ~TerrainManipulator();

    enum HeightBlendMode
    {
        Alpha = 0,
        Additive = 1
    };

    struct BrushOperationData
    {
        Unigine::MaterialPtr brush_material;
        Unigine::ImagePtr    albedo_image;  // For direct albedo overwrite
        float                brush_height;
        float                brush_size;
        float                brush_rotation;
        float                brush_opacity = 1;
        HeightBlendMode      height_blend_mode = HeightBlendMode::Alpha;
        bool                 modify_heights = false;
        bool                 modify_albedo = false;
        bool                 modify_mask = false;
        int                  mask_index = 0;
    };

    // Public terrain operations
    void RaiseTerrainToSurface(Unigine::NodePtr node, const std::string& surface_name,
                               Unigine::Math::dvec2 brush_size, Unigine::MaterialPtr brush_material,
                               bool target_albedo = false);
    void RaiseTerrainToVertices(std::vector<Unigine::Math::dvec3> verts,
                                Unigine::Math::dvec2 brush_size, Unigine::MaterialPtr brush_material);
    void RaiseTerrainToBoundingBox(Unigine::NodePtr node, Unigine::Math::dvec2 brush_size,
                                   Unigine::MaterialPtr brush_material);

    // Height image operations
    bool SetTerrainHeight(Unigine::LandscapeLayerMapPtr lmap, Unigine::ImagePtr height_image);

    // Mask image operations
    bool SetTerrainMask(Unigine::LandscapeLayerMapPtr lmap, Unigine::NodePtr node, Unigine::ImagePtr mask_image, Unigine::Math::dvec2 brush_size, int mask_index);
    void SetTerrainAlbedoImmediate(Unigine::LandscapeLayerMapPtr lmap, Unigine::UGUID guid, int id,
                                   Unigine::LandscapeTexturesPtr buffer, Unigine::Math::ivec2 coord,
                                   int data_mask, Unigine::ImagePtr albedo_image);

    // Status
    bool   IsTerrainManipulationInProgress() const;
    size_t NumberOfRemainingOperations() const;
    void   BlockBrushOperations(bool block);

    // Brush material creation
    static Unigine::MaterialPtr getCircularTerrainBrushWithFalloff(double falloff, double padding = 0,
                                    std::string DEFAULT = "circle_medium.brush");
    static Unigine::MaterialPtr getTerrainTileBrush(Unigine::ImagePtr height_image);
    static Unigine::MaterialPtr getMaskTerrainBrush(Unigine::ImagePtr mask_image);

private:
    void RaiseTerrainToVertex(Unigine::Math::dvec3 vertex, Unigine::Math::dvec2 brush_size,
                              Unigine::MaterialPtr brush_material, float brush_rotation = 0,
                              Unigine::LandscapeLayerMapPtr map_to_operate = nullptr,
                              bool target_albedo = false);

    void CalculateDrawingRegion(Unigine::Math::dvec3 brush_local_position,
                                Unigine::Math::quat brush_local_rotation,
                                Unigine::Math::dvec2 half_size,
                                Unigine::LandscapeLayerMapPtr closest_lmap,
                                Unigine::Math::ivec2& drawing_region_coord,
                                Unigine::Math::ivec2& drawing_region_size);

    void PerformTerrainManipulationOperation(Unigine::LandscapeLayerMapPtr lmap,
                                             Unigine::Math::ivec2 pixel_coord,
                                             Unigine::Math::ivec2 resolution,
                                             int flags);

    void OnTextureDraw(Unigine::UGUID guid, int id, Unigine::LandscapeTexturesPtr buffer,
                       Unigine::Math::ivec2 coord, int data_mask);

    void ApplyBrush(BrushOperationData const& operation, Unigine::UGUID guid, int id,
                    Unigine::LandscapeTexturesPtr buffer, Unigine::Math::ivec2 coord, int data_mask);

    std::queue<BrushOperationData> m_brush_buffer;
    bool m_in_progress = false;
    bool m_operations_blocked = false;
    int  m_lock_count = 0;
    Unigine::EventConnectionId m_event_connection_id = nullptr;
};

/* ========================================================================
 * Forward declarations
 * ======================================================================== */
class UiPanelTerrain;

namespace UnigineEditor
{

/* ========================================================================
 * TerrainSurfaceToolEditorPlugin - Main Editor2 Plugin
 * ======================================================================== */
class TerrainSurfaceToolEditorPlugin : public QObject, public Plugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID UNIGINE_EDITOR_PLUGIN_IID FILE "TerrainSurfaceToolEditorPlugin.json")
    Q_INTERFACES(UnigineEditor::Plugin)

public:
    TerrainSurfaceToolEditorPlugin();
    ~TerrainSurfaceToolEditorPlugin() override;

    bool init() override;
    void shutdown() override;

    // Public accessors for the UI panel
    std::vector<Unigine::NodePtr> getSelectedMeshNodes();
    TerrainManipulator* getManipulator() { return terrain_manipulator_.get(); }

private slots:
    void openTerrainTool();

private:
    void setupMenu();

    std::unique_ptr<UiPanelTerrain>    ui_panel_;
    std::unique_ptr<TerrainManipulator> terrain_manipulator_;

    QMenu*   vamps_menu_ = nullptr;
    QAction* terrain_tool_action_ = nullptr;
};

} // namespace UnigineEditor

/* ========================================================================
 * UiPanelTerrain - Qt docked panel (like GantryLabelTool UiPanelEditor)
 * ======================================================================== */
class UiPanelTerrain : public QWidget
{
    Q_OBJECT

public:
    explicit UiPanelTerrain(UnigineEditor::TerrainSurfaceToolEditorPlugin* plugin);
    ~UiPanelTerrain() override;

private slots:
    void onApplyPullTerrain();
    void onApplyToMask();
    void onSmoothTerrain();
    void onResetTerrainHeight();

private:
    void setupUI();
    void updateSelectionStatus();
    void logMessage(const QString& msg);

    UnigineEditor::TerrainSurfaceToolEditorPlugin* plugin_;

    // Selection
    QLabel*      selection_status_label_;

    // Surface filter
    QLineEdit*      edit_surface_name_;
    QLineEdit*      edit_mask_name_;

    // Brush settings
    QDoubleSpinBox* spin_brush_size_;
    QDoubleSpinBox* spin_flat_distance_;
    QDoubleSpinBox* spin_falloff_distance_;
    QDoubleSpinBox* spin_smooth_iterations_;
    QDoubleSpinBox* spin_smooth_strength_;

    // Action buttons
    QPushButton* btn_apply_;
    QPushButton* btn_apply_mask_;
    QPushButton* btn_smooth_;
    QPushButton* btn_reset_;

    // Progress & log
    QProgressBar* progress_bar_;
    QTextEdit*    status_text_;
};

/* ========================================================================
 * Inline helper functions (from TerrainHelper.h and GeometryHelper.h)
 * ======================================================================== */

// Test if terrain data can be fetched at the given 2D position
inline bool TerrainTestFetchAtPosition(Unigine::Math::dvec2 position)
{
    Unigine::ObjectLandscapeTerrainPtr terrain = Unigine::Landscape::getActiveTerrain();
    if (terrain == nullptr)
        return false;
    auto landscape_fetch = Unigine::LandscapeFetch::create();
    bool fetched = landscape_fetch->fetchForce(position);
    return fetched;
}

// Find the closest LandscapeLayerMap that contains the given world position
inline Unigine::LandscapeLayerMapPtr GetClosestLandscapeLayerMap(
    Unigine::ObjectLandscapeTerrainPtr terrain,
    const Unigine::Math::dvec3 test_position)
{
    Unigine::LandscapeLayerMapPtr closest_lmap = nullptr;
    if (terrain)
    {
        for (int i = 0; i < terrain->getNumChildren(); i++)
        {
            if (auto lmap = Unigine::checked_ptr_cast<Unigine::LandscapeLayerMap>(terrain->getChild(i));
                lmap != nullptr)
            {
                if (lmap->getWorldBoundBox().inside(test_position))
                {
                    if (!closest_lmap || lmap->getOrder() > closest_lmap->getOrder())
                        closest_lmap = lmap;
                }
            }
        }
    }
    return closest_lmap;
}

// Get surface vertices from an ObjectMeshStatic in world space
inline std::vector<Unigine::Math::vec3> GetSurfaceVerticesFromMeshInWorldSpace(
    Unigine::ObjectMeshStaticPtr mesh, std::string surface_name)
{
    std::vector<Unigine::Math::vec3> return_verts;
    auto world_transform = mesh->getWorldTransform();

    int surface_id = mesh->findSurface(surface_name.c_str());
    if (surface_id == -1)
        return return_verts;

    auto mesh_static = mesh->getMeshForce();
    auto num_of_vertices = mesh_static->getNumVertices(surface_id);
    for (int i = 0; i < num_of_vertices; i++)
    {
        auto mesh_vert = mesh_static->getVertex(i, surface_id);
        auto vert_in_world_space = world_transform * Unigine::Math::dvec3(mesh_vert.x, mesh_vert.y, mesh_vert.z);
        return_verts.push_back(Unigine::Math::vec3(
            (float)vert_in_world_space.x, (float)vert_in_world_space.y, (float)vert_in_world_space.z));
    }
    return return_verts;
}

// Sample points along a line at the given spacing
inline std::vector<Unigine::Math::vec3> SampleLine(
    const Unigine::Math::vec3& start,
    const Unigine::Math::vec3& end,
    const double spacing)
{
    if (spacing <= 0.0)
        return {};
    const auto dist = Unigine::Math::distance(start, end);
    if (dist < Unigine::Math::Consts::EPS_D)
        return { start };
    const auto dt = spacing / dist;
    std::vector<Unigine::Math::vec3> sampled_points;
    for (double t = 0.0; t < 1.0; t += dt)
        sampled_points.emplace_back(Unigine::Math::lerp(start, end, static_cast<float>(t)));
    sampled_points.emplace_back(end);
    return sampled_points;
}

// Barycentric point-in-triangle test
inline int pointInTriangle(Unigine::Math::Vec3 point, Unigine::Math::Vec3 v1,
    Unigine::Math::Vec3 v2, Unigine::Math::Vec3 v3, double& u, double& v)
{
    Unigine::Math::Vec3 v0 = v3 - v1;
    Unigine::Math::Vec3 e1 = v2 - v1;
    Unigine::Math::Vec3 e2 = point - v1;
    double dot00 = Unigine::Math::dot(v0, v0);
    double dot01 = Unigine::Math::dot(v0, e1);
    double dot02 = Unigine::Math::dot(v0, e2);
    double dot11 = Unigine::Math::dot(e1, e1);
    double dot12 = Unigine::Math::dot(e1, e2);
    double inv_denom = 1.0 / (dot00 * dot11 - dot01 * dot01);
    u = (dot11 * dot02 - dot01 * dot12) * inv_denom;
    v = (dot00 * dot12 - dot01 * dot02) * inv_denom;
    return (u > 0 && v > 0 && u + v < 1) ? 1 : 0;
}

// Extract vertices from an object surface into world space (triangle list)
inline size_t appendBaseVerts(ObjectSurface const& object_surface,
    Unigine::Vector<Unigine::Math::dvec3>& append_vertices_world_space)
{
    using namespace Unigine;
    using namespace Unigine::Math;

    dmat4 world_transform = object_surface.first->getWorldTransform();
    auto object = object_surface.first;
    auto surface_index = object_surface.second;

    if (object->getType() == Node::OBJECT_MESH_STATIC)
    {
        ObjectMeshStaticPtr oms = dynamic_ptr_cast<ObjectMeshStatic>(object);
        if (oms->isValid())
        {
            auto mesh = oms->getMeshForce();
            if (mesh->isValid())
            {
                Vector<int> const& indices = mesh->getCIndices(surface_index);
                Vector<vec3> const& verts = mesh->getVertices(surface_index);
                for (int idx : indices)
                {
                    dvec3 ws = world_transform * dvec3(verts[idx]);
                    append_vertices_world_space.push_back(ws);
                }
            }
        }
    }
    return append_vertices_world_space.size();
}

// Find the first surface whose name matches the regex
inline ObjectSurface getBaseSurface(Unigine::ObjectPtr object, std::regex surface_name_regex)
{
    int size = object->getNumSurfaces();
    for (int i = 0; i < size; i++)
    {
        std::string name = object->getSurfaceName(i);
        if (std::regex_search(name, surface_name_regex))
            return std::make_pair(object, i);
    }
    return std::make_pair(nullptr, 0);
}

// Flatten node hierarchy to a list of Objects
inline void flattenToObjectListHelper(Unigine::NodePtr node, Unigine::Vector<Unigine::ObjectPtr>& out)
{
    using namespace Unigine;
    if (!node) return;
    if (node->getType() == Node::OBJECT_MESH_STATIC || node->getType() == Node::OBJECT_MESH_DYNAMIC)
        out.push_back(checked_ptr_cast<Object>(node));
    else if (node->getType() == Node::NODE_REFERENCE)
    {
        NodeReferencePtr ref = checked_ptr_cast<NodeReference>(node);
        auto target = ref->getReference();
        if (target->isValid())
            flattenToObjectListHelper(target, out);
    }
    for (int i = 0; i < node->getNumChildren(); i++)
        flattenToObjectListHelper(node->getChild(i), out);
}

inline Unigine::Vector<Unigine::ObjectPtr> flattenToObjectList(Unigine::Vector<Unigine::NodePtr> nodes)
{
    Unigine::Vector<Unigine::ObjectPtr> out;
    for (const auto& n : nodes)
        flattenToObjectListHelper(n, out);
    return out;
}

// Filter objects that have a surface matching the regex
inline Unigine::Vector<ObjectSurface> filterMatchingObjects(
    Unigine::Vector<Unigine::ObjectPtr> objects, std::regex surface_name_regex)
{
    Unigine::Vector<ObjectSurface> result;
    for (auto obj : objects)
    {
        auto surface = getBaseSurface(obj, surface_name_regex);
        if (surface.first)
            result.push_back(surface);
    }
    return result;
}

// Get objects within a bounding box
inline std::vector<ObjectSurface> getObjectsWithinBoundingBox(
    const Unigine::Math::WorldBoundBox& wbb,
    const std::vector<ObjectSurface>& candidates, float radius = 0.0f)
{
    std::vector<ObjectSurface> result;
    for (auto pair : candidates)
    {
        Unigine::Math::WorldBoundBox obb = pair.first->getWorldBoundBox();
        obb.set(obb.minimum - radius, obb.maximum + radius);
        if (wbb.insideValid(obb))
            result.push_back(pair);
    }
    return result;
}

// Generate surface heights for a terrain tile from an object surface
inline bool GetSurfaceHeightsForTerrain(Unigine::LandscapeLayerMapPtr terrain_tile,
    ObjectSurface object_surface, Unigine::ImagePtr heights_image,
    std::map<int, int>& height_data)
{
    using namespace Unigine::Math;
    auto iworld = terrain_tile->getIWorldTransform();
    auto res = terrain_tile->getResolution();
    auto size = terrain_tile->getSize();
    auto step = size / Vec2(res.x, res.y);
    bool modified = false;

    Unigine::Vector<dvec3> verts_ws;
    size_t num_verts = appendBaseVerts(object_surface, verts_ws);

    for (int i = 2; i < (int)num_verts; i += 3)
    {
        Vec3 lv1 = iworld * Vec3(verts_ws[i]);
        Vec3 lv2 = iworld * Vec3(verts_ws[i - 1]);
        Vec3 lv3 = iworld * Vec3(verts_ws[i - 2]);
        Vec3 tv1 = Vec3(lv1.x, lv1.y, 0.0);
        Vec3 tv2 = Vec3(lv2.x, lv2.y, 0.0);
        Vec3 tv3 = Vec3(lv3.x, lv3.y, 0.0);

        int xg1 = int(lv1.x / step.x), yg1 = int(lv1.y / step.y);
        int xg2 = int(lv2.x / step.x), yg2 = int(lv2.y / step.y);
        int xg3 = int(lv3.x / step.x), yg3 = int(lv3.y / step.y);
        int mn_x = std::max(0, std::min({xg1, xg2, xg3}));
        int mn_y = std::max(0, std::min({yg1, yg2, yg3}));
        int mx_x = std::min(res.x - 1, std::max({xg1, xg2, xg3}));
        int mx_y = std::min(res.y - 1, std::max({yg1, yg2, yg3}));

        for (int x = mn_x; x <= mx_x; x++)
        {
            if (x < 0 || x >= res.x) continue;
            for (int y = mn_y; y <= mx_y; y++)
            {
                if (y < 0 || y >= res.y) continue;
                int index = x + res.x * y;
                if (heights_image->get2D(x, y).f.a <= 0.0)
                {
                    double u, v;
                    if (pointInTriangle(Vec3((x + 0.5f) * step.x, (y + 0.5f) * step.y, 0.0f),
                        tv1, tv2, tv3, u, v) == 1)
                    {
                        Vec3 pt = lv1 + ((lv3 - lv1) * (float)u) + ((lv2 - lv1) * (float)v);
                        Unigine::Image::Pixel pix;
                        pix.f.r = pt.z; pix.f.g = pt.z; pix.f.b = pt.z; pix.f.a = 1.0f;
                        // Flip Y coordinate for correct image positioning
                        heights_image->set2D(x, res.y - 1 - y, pix);
                        modified = true;
                        height_data[index] = index;
                    }
                }
            }
        }
    }
    return modified;
}

// Smooth terrain heights around tracked points
inline void smoothTerrainHeights(Unigine::LandscapeLayerMapPtr tile,
    Unigine::ImagePtr heights_image, std::map<int, int>& height_data,
    double flat_distance, double fall_off_distance);

// Apply falloff to mask image alpha channel
inline void applyMaskFalloff(Unigine::ImagePtr mask_image, std::map<int, int>& mask_data,
    double falloff_distance)
{
    using namespace Unigine::Math;
    auto res = Unigine::Math::ivec2(mask_image->getWidth(), mask_image->getHeight());
    
    int fall_off_steps = (int)falloff_distance;
    if (fall_off_steps < 1) fall_off_steps = 1;
    
    std::vector<int> indices;
    for (const auto& [pi, si] : mask_data)
        indices.push_back(pi);
    
    // Create a temporary alpha map
    std::vector<float> alpha_map(res.x * res.y, 0.0f);
    
    // Mark all painted pixels with alpha = 1
    for (int index : indices)
    {
        alpha_map[index] = 1.0f;
    }
    
    // Apply falloff by blending with neighboring pixels
    for (int index : indices)
    {
        int x = index % res.x;
        int y = index / res.x;
        
        // Check neighboring pixels and apply falloff
        for (int dx = -fall_off_steps; dx <= fall_off_steps; dx++)
        {
            for (int dy = -fall_off_steps; dy <= fall_off_steps; dy++)
            {
                int nx = x + dx;
                int ny = y + dy;
                
                if (nx < 0 || nx >= res.x || ny < 0 || ny >= res.y)
                    continue;
                
                int neighbor_index = nx + res.x * ny;
                float distance = sqrtf((float)(dx * dx + dy * dy));
                float falloff = 1.0f - Unigine::Math::clamp(distance / (float)falloff_distance, 0.0f, 1.0f);
                
                // Take the maximum alpha value
                alpha_map[neighbor_index] = Unigine::Math::max(alpha_map[neighbor_index], falloff);
            }
        }
    }
    
    // Apply the alpha map to the image (with Y-flip)
    for (int x = 0; x < res.x; x++)
    {
        for (int y = 0; y < res.y; y++)
        {
            int index = x + res.x * y;
            if (alpha_map[index] > 0.0f)
            {
                Unigine::Image::Pixel pix = mask_image->get2D(x, y);
                pix.f.a = alpha_map[index];
                // Flip Y when setting the pixel
                mask_image->set2D(x, res.y - 1 - y, pix);
            }
        }
    }
}

// Smooth terrain heights around tracked points
inline void smoothTerrainHeights(Unigine::LandscapeLayerMapPtr tile,
    Unigine::ImagePtr heights_image, std::map<int, int>& height_data,
    double flat_distance, double fall_off_distance)
{
    using namespace Unigine::Math;
    auto res = tile->getResolution();
    auto pixels_per_unit = Vec2(res) / Vec2(tile->getSize());

    auto fall_off_steps = ivec2(pixels_per_unit * (float)fall_off_distance).max();
    auto flat_steps = ivec2(pixels_per_unit * (float)flat_distance).max();
    auto total_steps = ivec2(pixels_per_unit * (float)(flat_distance + fall_off_distance)).max();

    std::vector<int> indices;
    for (const auto& [pi, si] : height_data)
        indices.push_back(pi);

    auto considerGridPoint = [&](int x, int y, int step, int pixel_index)
    {
        int new_index = x + res.x * y;
        if (height_data.count(new_index) == 0)
        {
            int ref_idx = height_data[pixel_index];
            int ref_x = ref_idx % res.x;
            int ref_y = ref_idx / res.x;
            auto ref_pix = heights_image->get2D(ref_x, ref_y);
            Unigine::Image::Pixel new_pix = ref_pix;
            if (step + 1 >= flat_steps)
            {
                double t = double(total_steps - step - 1) / double(fall_off_steps);
                double ss = 3.0 * std::pow(t, 2.0) - 2.0 * std::pow(t, 3.0);
                new_pix.f.a = static_cast<float>(ss);
            }
            heights_image->set2D(x, y, new_pix);
            height_data[new_index] = height_data[pixel_index];
            indices.push_back(new_index);
        }
    };

    for (int cs = 0; cs < total_steps; cs++)
    {
        std::vector<int> last_indices = indices;
        indices.clear();
        for (int pi : last_indices)
        {
            int x = pi % res.x;
            int y = pi / res.x;
            if (x + 1 < res.x) considerGridPoint(x + 1, y, cs, pi);
            if (x - 1 >= 0) considerGridPoint(x - 1, y, cs, pi);
            if (y + 1 < res.y) considerGridPoint(x, y + 1, cs, pi);
            if (y - 1 >= 0) considerGridPoint(x, y - 1, cs, pi);
        }
    }
}
