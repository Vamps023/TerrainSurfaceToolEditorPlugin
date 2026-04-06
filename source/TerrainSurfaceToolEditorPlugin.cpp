/* ========================================================================
 * TerrainSurfaceToolEditorPlugin.cpp
 *
 * Pure Editor2 Plugin - "Pull Terrain To Surface"
 * UI pattern: docked QWidget panel (same as GantryLabelTool)
 * ======================================================================== */

#define _USE_MATH_DEFINES
#include "TerrainSurfaceToolEditorPlugin.h"
#include <math.h>
#include <QtWidgets/QApplication>
#include <QtGui/QPalette>

using namespace Unigine;
using namespace Unigine::Math;

/* ========================================================================
 * TerrainManipulator implementation
 * ======================================================================== */

TerrainManipulator::TerrainManipulator()
{
    m_event_connection_id = Landscape::getEventTextureDraw().connect(
        [this](const UGUID& guid, int id, const LandscapeTexturesPtr& buffer,
            const Math::ivec2& coord, int data_mask)
        { OnTextureDraw(guid, id, buffer, coord, data_mask); });
}

TerrainManipulator::~TerrainManipulator()
{
    if (m_event_connection_id)
    {
        Landscape::getEventTextureDraw().disconnect(m_event_connection_id);
        m_event_connection_id = nullptr;
    }
}

bool TerrainManipulator::IsTerrainManipulationInProgress() const
{
    return m_in_progress;
}

size_t TerrainManipulator::NumberOfRemainingOperations() const
{
    return m_operations_blocked ? m_brush_buffer.size() + m_lock_count : m_brush_buffer.size();
}

void TerrainManipulator::BlockBrushOperations(bool block)
{
    m_operations_blocked = block;
}

/* --- RaiseTerrainToSurface ---
 * The main entry point: given a node and surface name, extract mesh vertices
 * in world space, compute midpoints, sample along the path, then raise terrain
 * at each sampled point. */
void TerrainManipulator::RaiseTerrainToSurface(
    NodePtr node, std::string surface_name,
    dvec2 brush_size, MaterialPtr brush_material)
{
    if (node->getType() != Node::OBJECT_MESH_STATIC)
    {
        Log::warning("RaiseTerrainToSurface: node is not OBJECT_MESH_STATIC\n");
        return;
    }

    auto static_mesh = static_ptr_cast<ObjectMeshStatic>(node);
    const auto surface_verts = GetSurfaceVerticesFromMeshInWorldSpace(static_mesh, surface_name);

    if (surface_verts.size() < 2)
    {
        Log::warning("RaiseTerrainToSurface: not enough vertices (%d) on surface '%s'\n",
                     (int)surface_verts.size(), surface_name.c_str());
        return;
    }

    Log::message("RaiseTerrainToSurface: found %d vertices on surface '%s'\n",
                 (int)surface_verts.size(), surface_name.c_str());

    // Compute midpoints between consecutive vertex pairs
    std::vector<vec3> mid_points;
    for (int i = 0; i < (int)surface_verts.size() - 1; i += 2)
    {
        auto p1 = surface_verts[i];
        auto p2 = surface_verts[i + 1];
        float midX = (p1.x + p2.x) / 2.0f;
        float midY = (p1.y + p2.y) / 2.0f;
        float midZ = (p1.z + p2.z) / 2.0f;
        mid_points.push_back(vec3(midX, midY, midZ));
    }

    if (mid_points.empty())
        return;

    // Sample along the midpoint path at brush_size intervals for smooth coverage
    std::vector<vec3> brush_locales;
    for (int i = 0; i < (int)mid_points.size() - 1; i++)
    {
        auto samples = SampleLine(mid_points[i], mid_points[i + 1], brush_size.x * 0.4f);
        brush_locales.push_back(mid_points[i]);
        brush_locales.insert(brush_locales.end(), samples.begin(), samples.end());
        brush_locales.push_back(mid_points[i + 1]);
    }
    // If only one midpoint, use it directly
    if (mid_points.size() == 1)
        brush_locales.push_back(mid_points[0]);

    // Calculate brush rotation from direction between consecutive points
    auto calculate_direction = [](vec3 x1, vec3 x2) -> double
    {
        double delta_x = x2.x - x1.x;
        double delta_y = x2.y - x1.y;
        double angle_rad = std::atan2(delta_y, delta_x);
        double angle_deg = angle_rad * 180.0 / M_PI;
        return angle_deg;
    };

    float brush_rotation = 0;
    for (int i = 0; i < (int)brush_locales.size(); i++)
    {
        auto v = brush_locales[i];

        if (i < (int)brush_locales.size() - 1)
        {
            auto v1 = brush_locales[i + 1];
            brush_rotation = static_cast<float>(calculate_direction(v, v1) + 90.0);
        }

        RaiseTerrainToVertex(dvec3(v.x, v.y, v.z), brush_size, brush_material, brush_rotation);
    }

    Log::message("RaiseTerrainToSurface: queued %d brush operations\n", (int)brush_locales.size());
}

/* --- RaiseTerrainToVertices --- */
void TerrainManipulator::RaiseTerrainToVertices(
    std::vector<dvec3> vertices, dvec2 brush_size, MaterialPtr brush_material)
{
    auto calculate_direction = [](dvec3 x1, dvec3 x2) -> double
    {
        double delta_x = x2.x - x1.x;
        double delta_y = x2.y - x1.y;
        return std::atan2(delta_y, delta_x) * 180.0 / M_PI;
    };

    double brush_rotation = 0;
    for (int i = 0; i < (int)vertices.size(); i++)
    {
        if (i < (int)vertices.size() - 1)
            brush_rotation = calculate_direction(vertices[i], vertices[i + 1]) + 90.0;

        RaiseTerrainToVertex(vertices[i], brush_size, brush_material,
                             static_cast<float>(brush_rotation));
    }
}

/* --- RaiseTerrainToBoundingBox --- */
void TerrainManipulator::RaiseTerrainToBoundingBox(
    NodePtr node, dvec2 brush_size, MaterialPtr brush_material)
{
    auto bb = node->getHierarchyWorldBoundBox();
    auto center = bb.getCenter();
    center.z = bb.minimum.z;
    RaiseTerrainToVertex(center, brush_size, brush_material, 0);
}

/* --- RaiseTerrainToVertex ---
 * Core per-vertex terrain modification. Finds the closest LandscapeLayerMap,
 * calculates the drawing region, queues a brush operation, and triggers
 * an async texture draw. */
void TerrainManipulator::RaiseTerrainToVertex(
    dvec3 vertex, dvec2 brush_size, MaterialPtr brush_material,
    float brush_rotation, LandscapeLayerMapPtr map_to_operate)
{
    if (!TerrainTestFetchAtPosition(dvec2{ vertex.x, vertex.y }))
        return;

    ObjectLandscapeTerrainPtr terrain{ Landscape::getActiveTerrain() };
    const auto d_position = dvec3(vertex);

    auto closest_lmap = GetClosestLandscapeLayerMap(terrain, d_position);
    if (map_to_operate)
        closest_lmap = map_to_operate;
    if (!closest_lmap)
        return;

    // Convert world position to the layer map's local coordinate space
    auto brush_local_position = closest_lmap->getIWorldTransform() * d_position;

    quat brush_world_rotation = quat(vec3_up, brush_rotation);
    quat brush_local_rotation = brush_world_rotation * inverse(closest_lmap->getWorldRotation());

    auto half_size = brush_size / 2.0f;

    ivec2 drawing_region_coord;
    ivec2 drawing_region_size;
    CalculateDrawingRegion(brush_local_position, brush_local_rotation, half_size,
                           closest_lmap, drawing_region_coord, drawing_region_size);

    auto pixels_per_unit = Vec2{ closest_lmap->getResolution() } / Vec2{ closest_lmap->getSize() };
    auto pixel_coord = ivec2{ pixels_per_unit * Vec2(brush_local_position) };

    Log::message("raising at tile loc (%g, %g) pixel (%d, %d) draw (%d, %d) size (%d %d)\n",
        brush_local_position.x, brush_local_position.y,
        pixel_coord.x, pixel_coord.y,
        drawing_region_coord.x, drawing_region_coord.y,
        drawing_region_size.x, drawing_region_size.y);

    // Queue the brush operation
    BrushOperationData data;
    data.brush_height = static_cast<float>(vertex.z);
    data.brush_size = static_cast<float>(max(drawing_region_size.x, drawing_region_size.y));
    data.brush_opacity = 1;
    data.height_blend_mode = HeightBlendMode::Alpha;
    data.brush_material = brush_material;
    data.brush_rotation = brush_rotation;
    data.modify_heights = true;

    m_brush_buffer.push(data);
    PerformTerrainManipulationOperation(closest_lmap, drawing_region_coord, drawing_region_size);

    // Handle brush operations that overfill a terrain chunk boundary
    auto max_x = closest_lmap->getSize().x - drawing_region_size.x;
    auto max_y = closest_lmap->getSize().y - drawing_region_size.y;
    if ((drawing_region_coord.x <= 0 || drawing_region_coord.x >= max_x ||
         drawing_region_coord.y <= 0 || drawing_region_coord.y >= max_y) &&
        map_to_operate == nullptr)
    {
        auto current_pos = closest_lmap->getWorldPosition();
        auto dws = dvec3();
        dws.x = drawing_region_coord.x <= 0
            ? current_pos.x + drawing_region_coord.x
            : current_pos.x + drawing_region_coord.x + drawing_region_size.x;
        dws.y = drawing_region_coord.y <= 0
            ? current_pos.y + drawing_region_coord.y
            : current_pos.y + drawing_region_coord.y + drawing_region_size.y;
        dws.z = d_position.z;

        auto op_lmap = GetClosestLandscapeLayerMap(terrain, dws);
        if (op_lmap == closest_lmap || map_to_operate == op_lmap)
            return;

        RaiseTerrainToVertex(vertex, brush_size, brush_material, brush_rotation, op_lmap);
    }
}

/* --- SetTerrainHeight --- */
bool TerrainManipulator::SetTerrainHeight(LandscapeLayerMapPtr lmap, ImagePtr height_image)
{
    if (!lmap || !height_image)
        return false;

    auto brush = getTerrainTileBrush(height_image);
    if (!brush)
        return false;

    BrushOperationData data;
    data.brush_height = 1;
    data.brush_size = 1;
    data.brush_rotation = 0;
    data.brush_opacity = 1;
    data.height_blend_mode = HeightBlendMode::Alpha;
    data.brush_material = brush;
    data.modify_heights = true;
    m_brush_buffer.push(data);

    PerformTerrainManipulationOperation(lmap, ivec2{ 0, 0 }, lmap->getResolution());
    return true;
}

/* --- CalculateDrawingRegion ---
 * Given a brush position and size in local space, compute the pixel coordinates
 * and size of the region to be modified on the LandscapeLayerMap. */
void TerrainManipulator::CalculateDrawingRegion(
    dvec3 brush_local_position, quat brush_local_rotation, dvec2 half_size,
    LandscapeLayerMapPtr closest_lmap,
    ivec2& drawing_region_coord, ivec2& drawing_region_size)
{
    Vec3 corners[4] = {
        brush_local_position + brush_local_rotation * Vec3(-half_size.x, -half_size.y, 0.0),
        brush_local_position + brush_local_rotation * Vec3( half_size.x, -half_size.y, 0.0),
        brush_local_position + brush_local_rotation * Vec3(-half_size.x,  half_size.y, 0.0),
        brush_local_position + brush_local_rotation * Vec3( half_size.x,  half_size.y, 0.0)
    };

    auto bbox_min = Vec2{
        min(min(corners[0].x, corners[1].x), min(corners[2].x, corners[3].x)),
        min(min(corners[0].y, corners[1].y), min(corners[2].y, corners[3].y))
    };
    auto bbox_max = Vec2{
        max(max(corners[0].x, corners[1].x), max(corners[2].x, corners[3].x)),
        max(max(corners[0].y, corners[1].y), max(corners[2].y, corners[3].y))
    };

    auto pixels_per_unit = Vec2{ closest_lmap->getResolution() } / Vec2{ closest_lmap->getSize() };

    drawing_region_coord = ivec2{ round(pixels_per_unit * bbox_min) };
    drawing_region_size = ivec2{ pixels_per_unit * round(bbox_max - bbox_min) };
}

/* --- PerformTerrainManipulationOperation ---
 * Generate an operation ID and request an async texture draw. */
void TerrainManipulator::PerformTerrainManipulationOperation(
    LandscapeLayerMapPtr lmap, ivec2 pixel_coord, ivec2 resolution)
{
    int id = Landscape::generateOperationID();
    Landscape::asyncTextureDraw(id, lmap->getGUID(), pixel_coord, resolution,
                                Landscape::FLAGS_DATA_HEIGHT | Landscape::FLAGS_DATA_ALBEDO);
}

/* --- OnTextureDraw ---
 * Callback invoked by Landscape when an async texture draw operation completes.
 * Dequeues the next brush operation and applies it. */
void TerrainManipulator::OnTextureDraw(
    UGUID guid, int id, LandscapeTexturesPtr buffer, ivec2 coord, int data_mask)
{
    if (NumberOfRemainingOperations() == 0)
    {
        Log::message("No data to process, exiting terrain sculpt\n");
        m_in_progress = false;
        return;
    }

    m_in_progress = true;

    if (m_operations_blocked)
        return;

    if (m_brush_buffer.size() > 0)
    {
        BrushOperationData operation = m_brush_buffer.front();
        m_brush_buffer.pop();
        ApplyBrush(operation, guid, id, buffer, coord, data_mask);
    }

    if (NumberOfRemainingOperations() == 0)
    {
        Log::message("Finished Terrain Sculpting\n");
        m_in_progress = false;
    }
}

/* --- ApplyBrush ---
 * Configure the brush material with terrain data textures and parameters,
 * then execute the brush expression to modify the terrain. */
void TerrainManipulator::ApplyBrush(
    BrushOperationData const& operation, UGUID guid, int id,
    LandscapeTexturesPtr buffer, ivec2 coord, int data_mask)
{
    auto brush_material = operation.brush_material;

    if (operation.modify_albedo)
        brush_material->setTexture("terrain_albedo", buffer->getAlbedo());

    if (operation.modify_heights)
        brush_material->setTexture("terrain_height", buffer->getHeight());

    brush_material->setParameterFloat("size", operation.brush_size);
    brush_material->setParameterFloat("height", operation.brush_height);
    brush_material->setParameterFloat("angle", operation.brush_rotation);
    brush_material->setParameterFloat("opacity", operation.brush_opacity);
    brush_material->setParameterInt("data_mask", data_mask);
    brush_material->setState("height_blend_mode", static_cast<int>(operation.height_blend_mode));

    // Execute the brush shader expression
    brush_material->runExpression("brush", buffer->getResolution().x, buffer->getResolution().y);

    // Reset textures
    brush_material->setTexture("terrain_height", nullptr);
    brush_material->setTexture("terrain_albedo", nullptr);
}

/* --- getCircularTerrainBrushWithFalloff ---
 * Create a circular brush material with falloff for terrain height operations. */
MaterialPtr TerrainManipulator::getCircularTerrainBrushWithFalloff(
    double falloff, double padding, std::string DEFAULT)
{
    auto file_guid = FileSystem::getGUID(FileSystem::resolvePartialVirtualPath(DEFAULT.c_str()));
    if (!file_guid.isValid())
    {
        Log::warning("Cannot find brush material: %s\n", DEFAULT.c_str());
        return nullptr;
    }

    auto brush_material = Materials::findMaterialByFileGUID(file_guid)->inherit();

    ImagePtr image = Image::create();
    int width = 512;
    int height = 512;
    image->create2D(width, height, Image::FORMAT_RGBA8);

    float FALLOFF_AS_RATIO = Math::clamp((float)falloff, 0.0f, 1.0f);
    int centerX = width / 2;
    int centerY = height / 2;
    float radius = (width - (float)padding * width) / 2.0f;

    Image::Pixel pix;
    pix.i.r = 255; pix.i.g = 255; pix.i.b = 255; pix.i.a = 255;

    auto calcPct = [](float x1, float x2, float y1) -> float
    {
        return (y1 - x1) / (x2 - x1);
    };

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            float dist = sqrtf((float)((x - centerX) * (x - centerX) + (y - centerY) * (y - centerY)));

            if (dist > radius)
            {
                pix.i.a = 0;
                image->set2D(x, y, pix);
                continue;
            }
            float ratio = calcPct(0, radius, dist);
            if (ratio < FALLOFF_AS_RATIO)
            {
                pix.i.a = 255;
                image->set2D(x, y, pix);
                continue;
            }
            float fd = radius * FALLOFF_AS_RATIO;
            float fr = calcPct(fd, radius, dist);
            float inv = 1.0f - fr;
            int alpha_int = Math::clamp(static_cast<int>(255.0f * inv), 0, 255);
            pix.i.a = alpha_int;
            image->set2D(x, y, pix);
        }
    }

    UGUID gui = UGUID();
    gui.generate();

    std::string temp_folder = std::string(FileSystem::getRootMount()->getDataPath()) + "/world_builder/temp/";
    std::stringstream name_ss;
    name_ss << temp_folder << gui.getString() << ".png";
    image->save(name_ss.str().c_str());
    brush_material->setTexturePath("opacity", name_ss.str().c_str());

    return brush_material;
}

/* --- getTerrainTileBrush ---
 * Create a brush material from a height image. */
MaterialPtr TerrainManipulator::getTerrainTileBrush(ImagePtr height_image)
{
    if (!height_image)
        return nullptr;

    static const std::vector<std::string> brush_candidates = {
        "brush_without_contrast.basebrush",
        "circle_medium.brush",
        "circle_soft.brush",
        "circle_hard.brush"
    };

    UGUID file_guid;
    std::string resolved_brush_name;
    for (const auto& brush_name : brush_candidates)
    {
        auto guid = FileSystem::getGUID(FileSystem::resolvePartialVirtualPath(brush_name.c_str()));
        if (guid.isValid())
        {
            file_guid = guid;
            resolved_brush_name = brush_name;
            break;
        }
    }

    if (!file_guid.isValid())
    {
        static bool warned_missing_once = false;
        if (!warned_missing_once)
        {
            Log::warning("Could not find any tile brush material. Tried: brush_without_contrast.basebrush, circle_medium.brush, circle_soft.brush, circle_hard.brush\n");
            warned_missing_once = true;
        }
        return nullptr;
    }

    auto brush_material = Materials::findMaterialByFileGUID(file_guid)->inherit();
    auto texture_index = brush_material->findTexture("opacity");
    if (texture_index < 0)
    {
        Log::warning("Brush material '%s' has no 'opacity' texture slot\n", resolved_brush_name.c_str());
        return nullptr;
    }

    brush_material->setTextureImage(texture_index, height_image);
    return brush_material;
}

/* ========================================================================
 * TerrainSurfaceToolEditorPlugin implementation (GantryLabelTool pattern)
 * ======================================================================== */

namespace UnigineEditor
{

TerrainSurfaceToolEditorPlugin::TerrainSurfaceToolEditorPlugin()
    : ui_panel_(nullptr), terrain_manipulator_(nullptr)
{
    Log::message("[TerrainSurfaceTool] Constructor called!\n");
}

TerrainSurfaceToolEditorPlugin::~TerrainSurfaceToolEditorPlugin() = default;

bool TerrainSurfaceToolEditorPlugin::init()
{
    Log::message("[TerrainSurfaceTool] INIT() called!\n");

    try
    {
        terrain_manipulator_ = std::make_unique<TerrainManipulator>();
        ui_panel_ = std::make_unique<UiPanelTerrain>(this);

        Log::message("[TerrainSurfaceTool] UI panel created\n");

        setupMenu();

        Log::message("[TerrainSurfaceTool] Menu setup complete\n");

        WindowManager::add(ui_panel_.get(), WindowManager::AreaType::ROOT_AREA_RIGHT);

        Log::message("[TerrainSurfaceTool] Plugin initialized successfully\n");
        return true;
    }
    catch (const std::exception& e)
    {
        Log::error("[TerrainSurfaceTool] Failed to initialize: %s\n", e.what());
        return false;
    }
}

void TerrainSurfaceToolEditorPlugin::shutdown()
{
    Log::message("[TerrainSurfaceTool] Plugin shutting down...\n");

    if (vamps_menu_ && terrain_tool_action_)
        vamps_menu_->removeAction(terrain_tool_action_);

    if (ui_panel_)
        WindowManager::remove(ui_panel_.get());

    ui_panel_.reset();
    terrain_manipulator_.reset();

    Log::message("[TerrainSurfaceTool] Plugin shutdown complete\n");
}

std::vector<NodePtr> TerrainSurfaceToolEditorPlugin::getSelectedMeshNodes()
{
    std::vector<NodePtr> result;
    const SelectorNodes* selector = Selection::getSelectorNodes();
    if (!selector) return result;

    const Vector<NodePtr>& nodes = selector->getNodes();
    for (const auto& node : nodes)
    {
        if (!node) continue;
        if (node->getType() == Node::OBJECT_MESH_STATIC ||
            node->getType() == Node::OBJECT_MESH_DYNAMIC)
            result.push_back(node);
        // Also recurse into children for node references
        for (int i = 0; i < node->getNumChildren(); i++)
        {
            auto child = node->getChild(i);
            if (child && (child->getType() == Node::OBJECT_MESH_STATIC ||
                          child->getType() == Node::OBJECT_MESH_DYNAMIC))
                result.push_back(child);
        }
    }
    return result;
}

void TerrainSurfaceToolEditorPlugin::setupMenu()
{
    vamps_menu_ = WindowManager::findMenu("VampsPlugin");
    if (!vamps_menu_)
    {
        vamps_menu_ = new QMenu("VampsPlugin");
        Log::message("[TerrainSurfaceTool] Created VampsPlugin menu\n");
    }

    terrain_tool_action_ = new QAction("Terrain Surface Tool", this);
    connect(terrain_tool_action_, &QAction::triggered,
            this, &TerrainSurfaceToolEditorPlugin::openTerrainTool);

    if (vamps_menu_)
        vamps_menu_->addAction(terrain_tool_action_);
}

void TerrainSurfaceToolEditorPlugin::openTerrainTool()
{
    if (ui_panel_)
    {
        WindowManager::show(ui_panel_.get());
        WindowManager::activate(ui_panel_.get());
    }
}

} // namespace UnigineEditor

/* ========================================================================
 * UiPanelTerrain implementation
 * ======================================================================== */

UiPanelTerrain::UiPanelTerrain(UnigineEditor::TerrainSurfaceToolEditorPlugin* plugin)
    : QWidget()
    , plugin_(plugin)
{
    setupUI();
    updateSelectionStatus();

    setWindowTitle("Terrain Surface Tool");
    resize(400, 700);

    // Apply dark theme (same as GantryLabelTool)
    QPalette dark_palette;
    dark_palette.setColor(QPalette::Window, QColor(45, 45, 45));
    dark_palette.setColor(QPalette::WindowText, Qt::white);
    dark_palette.setColor(QPalette::Base, QColor(35, 35, 35));
    dark_palette.setColor(QPalette::AlternateBase, QColor(45, 45, 45));
    dark_palette.setColor(QPalette::ToolTipBase, Qt::white);
    dark_palette.setColor(QPalette::ToolTipText, Qt::white);
    dark_palette.setColor(QPalette::Text, Qt::white);
    dark_palette.setColor(QPalette::Button, QColor(60, 60, 60));
    dark_palette.setColor(QPalette::ButtonText, Qt::white);
    dark_palette.setColor(QPalette::BrightText, Qt::red);
    dark_palette.setColor(QPalette::Link, QColor(42, 130, 218));
    dark_palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
    dark_palette.setColor(QPalette::HighlightedText, Qt::black);
    setPalette(dark_palette);
}

UiPanelTerrain::~UiPanelTerrain() = default;

void UiPanelTerrain::setupUI()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setSpacing(10);
    main_layout->setContentsMargins(15, 15, 15, 15);

    // --- Header ---
    auto* header = new QLabel("Terrain Surface Tool", this);
    QFont header_font = header->font();
    header_font.setBold(true);
    header_font.setPointSize(14);
    header->setFont(header_font);
    main_layout->addWidget(header);

    // --- Selection Status ---
    selection_status_label_ = new QLabel("No mesh nodes selected", this);
    selection_status_label_->setStyleSheet("color: #ffcc00;");
    main_layout->addWidget(selection_status_label_);

    
    // --- Surface Filter ---
    auto* surface_group = new QGroupBox("Surface Filter", this);
    auto* surface_layout = new QHBoxLayout(surface_group);
    surface_layout->addWidget(new QLabel("Name/Regex:"));
    edit_surface_name_ = new QLineEdit("base", this);
    edit_surface_name_->setToolTip("Surface name or regex pattern to match on selected meshes");
    surface_layout->addWidget(edit_surface_name_);
    main_layout->addWidget(surface_group);

    // --- Brush Settings ---
    auto* brush_group = new QGroupBox("Brush Settings", this);
    auto* brush_layout = new QVBoxLayout(brush_group);

    auto addSpinRow = [&](const QString& label, double min_val, double max_val,
                          double default_val, double step, const QString& tip) -> QDoubleSpinBox*
    {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(label));
        auto* spin = new QDoubleSpinBox(this);
        spin->setRange(min_val, max_val);
        spin->setValue(default_val);
        spin->setSingleStep(step);
        spin->setToolTip(tip);
        row->addWidget(spin);
        brush_layout->addLayout(row);
        return spin;
    };

    spin_brush_size_ = addSpinRow("Brush Size:", 0.1, 1000.0, 10.0, 1.0,
        "Size of the terrain brush in world units");
    spin_flat_distance_ = addSpinRow("Flat Distance:", 0.0, 500.0, 30.0, 5.0,
        "Distance from track to flatten terrain (pixels)");
    spin_falloff_distance_ = addSpinRow("Falloff Distance:", 0.0, 2000.0, 30.0, 5.0,
        "Distance over which terrain blends back (pixels)");
    spin_smooth_iterations_ = addSpinRow("Smooth Passes:", 1.0, 50.0, 3.0, 1.0,
        "Number of smoothing passes");
    spin_smooth_strength_ = addSpinRow("Smooth Strength:", 0.0, 1.0, 0.5, 0.1,
        "Strength of each smoothing pass (0=none, 1=max)");

    main_layout->addWidget(brush_group);

    // --- Action Buttons ---
    auto* actions_group = new QGroupBox("Actions", this);
    auto* actions_layout = new QVBoxLayout(actions_group);

    btn_apply_ = new QPushButton("Apply Pull Terrain", this);
    btn_apply_->setStyleSheet("QPushButton { background-color: #2a7f2a; color: white; padding: 10px; font-weight: bold; }");
    connect(btn_apply_, &QPushButton::clicked, this, &UiPanelTerrain::onApplyPullTerrain);
    actions_layout->addWidget(btn_apply_);

    btn_smooth_ = new QPushButton("Smooth Terrain Around Selection", this);
    btn_smooth_->setStyleSheet("QPushButton { background-color: #2a6f8f; color: white; padding: 8px; }");
    connect(btn_smooth_, &QPushButton::clicked, this, &UiPanelTerrain::onSmoothTerrain);
    actions_layout->addWidget(btn_smooth_);

    btn_reset_ = new QPushButton("Reset / Clear Heights", this);
    btn_reset_->setStyleSheet("QPushButton { background-color: #8f2a2a; color: white; padding: 8px; }");
    connect(btn_reset_, &QPushButton::clicked, this, &UiPanelTerrain::onResetTerrainHeight);
    actions_layout->addWidget(btn_reset_);

    main_layout->addWidget(actions_group);

    // --- Progress ---
    progress_bar_ = new QProgressBar(this);
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    main_layout->addWidget(progress_bar_);

    // --- Status / Log ---
    main_layout->addWidget(new QLabel("Status:"));
    status_text_ = new QTextEdit(this);
    status_text_->setMaximumHeight(150);
    status_text_->setReadOnly(true);
    status_text_->setPlainText("Ready. Select mesh nodes in editor and click Apply.");
    main_layout->addWidget(status_text_);

    main_layout->addStretch();
}

void UiPanelTerrain::updateSelectionStatus()
{
    if (!plugin_) return;
    auto nodes = plugin_->getSelectedMeshNodes();
    if (nodes.empty())
    {
        selection_status_label_->setText("No mesh nodes selected");
        selection_status_label_->setStyleSheet("color: #ff6666;");
    }
    else
    {
        selection_status_label_->setText(QString("%1 mesh node(s) selected").arg(nodes.size()));
        selection_status_label_->setStyleSheet("color: #66ff66;");
    }
}

void UiPanelTerrain::logMessage(const QString& msg)
{
    if (status_text_)
    {
        status_text_->append(msg);
        status_text_->verticalScrollBar()->setValue(status_text_->verticalScrollBar()->maximum());
    }
    Unigine::Log::message("%s\n", msg.toUtf8().constData());
}


/* --- Apply Pull Terrain --- */
void UiPanelTerrain::onApplyPullTerrain()
{
    using namespace Unigine;
    using namespace Unigine::Math;

    logMessage("=== Pull Terrain To Surface ===");
    progress_bar_->setValue(0);

    auto selected = plugin_->getSelectedMeshNodes();
    if (selected.empty())
    {
        logMessage("ERROR: No mesh nodes selected.");
        return;
    }

    logMessage(QString("Found %1 mesh node(s)").arg(selected.size()));

    std::string surface_name = edit_surface_name_->text().toStdString();
    if (surface_name.empty())
    {
        logMessage("ERROR: Surface name is empty.");
        return;
    }

    double brush_size_val = spin_brush_size_->value();
    double flat_distance  = spin_flat_distance_->value();
    double falloff_dist   = spin_falloff_distance_->value();

    logMessage(QString("Surface: '%1', Brush: %2, Flat: %3, Falloff: %4")
        .arg(QString::fromStdString(surface_name))
        .arg(brush_size_val).arg(flat_distance).arg(falloff_dist));

    ObjectLandscapeTerrainPtr terrain = Landscape::getActiveTerrain();
    if (!terrain)
    {
        logMessage("ERROR: No active landscape terrain.");
        return;
    }

    MaterialPtr brush_material = TerrainManipulator::getCircularTerrainBrushWithFalloff(0.5);
    if (!brush_material)
        logMessage("WARNING: Could not create brush material. Using tile approach.");

    progress_bar_->setValue(10);
    int total = (int)selected.size();
    int processed = 0;

    for (auto& node : selected)
    {
        std::string node_name = node->getName();
        logMessage(QString("Processing: %1").arg(QString::fromStdString(node_name)));

        if (node->getType() == Node::OBJECT_MESH_STATIC)
        {
            auto oms = static_ptr_cast<ObjectMeshStatic>(node);

            int num_surfaces = oms->getNumSurfaces();
            logMessage(QString("  Surfaces (%1):").arg(num_surfaces));
            for (int si = 0; si < num_surfaces; si++)
                logMessage(QString("    [%1] '%2'").arg(si).arg(oms->getSurfaceName(si)));

            int surface_id = oms->findSurface(surface_name.c_str());

            if (surface_id >= 0)
            {
                logMessage(QString("  Found surface '%1' (id=%2)")
                    .arg(QString::fromStdString(surface_name)).arg(surface_id));

                if (brush_material)
                {
                    logMessage("  Using brush-based terrain modification...");
                    plugin_->getManipulator()->RaiseTerrainToSurface(
                        node, surface_name,
                        dvec2(brush_size_val, brush_size_val), brush_material);
                }
                else
                {
                    logMessage("  Using tile-based terrain modification...");
                    ObjectSurface obj_surface = std::make_pair(
                        static_ptr_cast<Object>(oms), surface_id);

                    for (int t = 0; t < terrain->getNumChildren(); t++)
                    {
                        auto lmap = checked_ptr_cast<LandscapeLayerMap>(terrain->getChild(t));
                        if (!lmap) continue;
                        if (!lmap->getWorldBoundBox().insideValid(oms->getWorldBoundBox()))
                            continue;

                        logMessage(QString("  Terrain tile %1...").arg(t));
                        auto tile_res = lmap->getResolution();
                        ImagePtr heights_image = Image::create();
                        heights_image->create2D(tile_res.x, tile_res.y, Image::FORMAT_RGBA32F);

                        Image::Pixel zp; zp.f.r=0; zp.f.g=0; zp.f.b=0; zp.f.a=0;
                        for (int x = 0; x < tile_res.x; x++)
                            for (int y = 0; y < tile_res.y; y++)
                                heights_image->set2D(x, y, zp);

                        std::map<int,int> height_data;
                        if (GetSurfaceHeightsForTerrain(lmap, obj_surface, heights_image, height_data))
                        {
                            logMessage(QString("  Modified %1 points").arg(height_data.size()));
                            smoothTerrainHeights(lmap, heights_image, height_data,
                                                 flat_distance, falloff_dist);
                            if (plugin_->getManipulator()->SetTerrainHeight(lmap, heights_image))
                                logMessage("  Tile updated.");
                            else
                                logMessage("  WARNING: Failed to apply tile update (brush material missing).");
                        }
                    }
                }
            }
            else
            {
                logMessage(QString("  '%1' not found, trying regex...").arg(QString::fromStdString(surface_name)));
                try
                {
                    std::regex rx(surface_name);
                    bool matched = false;
                    for (int s = 0; s < oms->getNumSurfaces(); s++)
                    {
                        std::string sn = oms->getSurfaceName(s);
                        if (std::regex_search(sn, rx))
                        {
                            matched = true;
                            logMessage(QString("  Regex matched: '%1' (id=%2)")
                                .arg(QString::fromStdString(sn)).arg(s));
                            if (brush_material)
                                plugin_->getManipulator()->RaiseTerrainToSurface(
                                    node, sn, dvec2(brush_size_val, brush_size_val), brush_material);
                        }
                    }
                    if (!matched)
                        logMessage("  WARNING: No surfaces matched. Check names above.");
                }
                catch (const std::regex_error& e)
                {
                    logMessage(QString("  Regex error: %1").arg(e.what()));
                }
            }
        }

        processed++;
        progress_bar_->setValue(10 + (processed * 80) / total);
    }

    progress_bar_->setValue(100);
    logMessage("=== Pull Terrain Complete ===");
}

/* --- Smooth Terrain Around Selection --- */
void UiPanelTerrain::onSmoothTerrain()
{
    using namespace Unigine;
    using namespace Unigine::Math;

    logMessage("=== Smooth Terrain ===");
    progress_bar_->setValue(0);

    auto selected = plugin_->getSelectedMeshNodes();
    if (selected.empty())
    {
        logMessage("ERROR: No mesh nodes selected.");
        return;
    }

    ObjectLandscapeTerrainPtr terrain = Landscape::getActiveTerrain();
    if (!terrain)
    {
        logMessage("ERROR: No active landscape terrain.");
        return;
    }

    std::string surface_name = edit_surface_name_->text().toStdString();
    double flat_dist   = spin_flat_distance_->value();
    double falloff     = spin_falloff_distance_->value();
    int    passes      = (int)spin_smooth_iterations_->value();
    double strength    = spin_smooth_strength_->value();

    logMessage(QString("Passes: %1, Strength: %2").arg(passes).arg(strength));

    int total = (int)selected.size();
    int processed = 0;

    for (auto& node : selected)
    {
        if (node->getType() != Node::OBJECT_MESH_STATIC) continue;
        auto oms = static_ptr_cast<ObjectMeshStatic>(node);

        int sid = oms->findSurface(surface_name.c_str());
        if (sid < 0)
        {
            // try first surface
            if (oms->getNumSurfaces() > 0) sid = 0;
            else continue;
        }

        ObjectSurface obj_surface = std::make_pair(static_ptr_cast<Object>(oms), sid);

        for (int t = 0; t < terrain->getNumChildren(); t++)
        {
            auto lmap = checked_ptr_cast<LandscapeLayerMap>(terrain->getChild(t));
            if (!lmap) continue;
            if (!lmap->getWorldBoundBox().insideValid(oms->getWorldBoundBox()))
                continue;

            auto tile_res = lmap->getResolution();
            ImagePtr heights_image = Image::create();
            heights_image->create2D(tile_res.x, tile_res.y, Image::FORMAT_RGBA32F);

            Image::Pixel zp; zp.f.r=0; zp.f.g=0; zp.f.b=0; zp.f.a=0;
            for (int x = 0; x < tile_res.x; x++)
                for (int y = 0; y < tile_res.y; y++)
                    heights_image->set2D(x, y, zp);

            std::map<int,int> height_data;
            if (GetSurfaceHeightsForTerrain(lmap, obj_surface, heights_image, height_data))
            {
                // Multiple smooth passes with adjustable strength
                for (int p = 0; p < passes; p++)
                    smoothTerrainHeights(lmap, heights_image, height_data,
                                         flat_dist * strength, falloff * strength);

                if (plugin_->getManipulator()->SetTerrainHeight(lmap, heights_image))
                    logMessage(QString("  Smoothed tile %1 (%2 passes)").arg(t).arg(passes));
                else
                    logMessage(QString("  WARNING: Failed to smooth tile %1 (brush material missing)").arg(t));
            }
        }

        processed++;
        progress_bar_->setValue((processed * 100) / total);
    }

    progress_bar_->setValue(100);
    logMessage("=== Smooth Complete ===");
}

/* --- Reset / Clear Terrain Heights --- */
void UiPanelTerrain::onResetTerrainHeight()
{
    using namespace Unigine;
    using namespace Unigine::Math;

    logMessage("=== Reset Terrain Heights ===");
    progress_bar_->setValue(0);

    auto selected = plugin_->getSelectedMeshNodes();
    if (selected.empty())
    {
        logMessage("ERROR: No mesh nodes selected.");
        return;
    }

    ObjectLandscapeTerrainPtr terrain = Landscape::getActiveTerrain();
    if (!terrain)
    {
        logMessage("ERROR: No active landscape terrain.");
        return;
    }

    int total = (int)selected.size();
    int processed = 0;

    for (auto& node : selected)
    {
        if (node->getType() != Node::OBJECT_MESH_STATIC) continue;
        auto oms = static_ptr_cast<ObjectMeshStatic>(node);

        for (int t = 0; t < terrain->getNumChildren(); t++)
        {
            auto lmap = checked_ptr_cast<LandscapeLayerMap>(terrain->getChild(t));
            if (!lmap) continue;
            if (!lmap->getWorldBoundBox().insideValid(oms->getWorldBoundBox()))
                continue;

            auto tile_res = lmap->getResolution();
            ImagePtr flat_image = Image::create();
            flat_image->create2D(tile_res.x, tile_res.y, Image::FORMAT_RGBA32F);

            // Set all heights to 0 with full alpha (= flatten to zero)
            Image::Pixel fp; fp.f.r = 0; fp.f.g = 0; fp.f.b = 0; fp.f.a = 1.0f;
            for (int x = 0; x < tile_res.x; x++)
                for (int y = 0; y < tile_res.y; y++)
                    flat_image->set2D(x, y, fp);

            if (plugin_->getManipulator()->SetTerrainHeight(lmap, flat_image))
                logMessage(QString("  Reset tile %1 to zero height").arg(t));
            else
                logMessage(QString("  WARNING: Failed to reset tile %1 (brush material missing)").arg(t));
        }

        processed++;
        progress_bar_->setValue((processed * 100) / total);
    }

    progress_bar_->setValue(100);
    logMessage("=== Reset Complete ===");
}
