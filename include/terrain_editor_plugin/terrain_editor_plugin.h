#pragma once

#include <UnigineMathLib.h>
#include <UnigineNode.h>
#include <UniginePlugin.h>
#include <UnigineWidgets.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <optional>

#include "terrain_creation.h"
#include "terrain_painting.h"
#include "terrain_to_surface.h"


namespace TerrainEditorPlugin
{
    class TerrainEditorSystemLogic;
    class TerrainEditorWorldLogic;
}

class terrain_creator;
class terrain_manipulator;
class CollapsibleWidget;

class terrain_creation_gui;
class import_ground_height_gui;
class import_ground_image_gui;

  /// Provides the functionality of the terrain_editor_plugin class, as a plugin for Unigine.
class terrain_editor_plugin : public Unigine::Plugin
{
public:
    terrain_editor_plugin();

    // Unigine::Plugin overrides:
    int init() override;
    int shutdown() override;
    void update() override;

    const char* get_name() override; 
    virtual int get_order() override;

private:

    std::unique_ptr<TerrainEditorPlugin::TerrainEditorSystemLogic> m_system_logic;
    std::unique_ptr<TerrainEditorPlugin::TerrainEditorWorldLogic>  m_world_logic;

    std::unique_ptr<terrain_manipulator> m_terrain_manipulator;
    std::unique_ptr<terrain_creator>     m_terrain_creator;

    size_t m_last_frame_operations = 0;

    bool m_terrain_painting_mode_enabled = false;

public:
    Unigine::WidgetPtr GetGUI();
    Unigine::WidgetPtr CreateGUI();

private:
    // GUI Elements
    Unigine::WidgetVBoxPtr        m_terrain_group_gui;
    // these should probably be owned by an appropriate managing class (see terrain_painting)
    std::shared_ptr<import_ground_height_gui> m_import_ground_height_gui;
    std::shared_ptr<terrain_creation_gui> m_terrain_creation_gui;
    std::shared_ptr<import_ground_image_gui> m_import_ground_image_gui;
    
    // not sure these need to be unique pointer as we dont use them anywhere else, they could potentially just be owned child classes?
    std::unique_ptr<terrain_creation> m_terrain_creation;
    std::unique_ptr<terrain_painting> m_terrain_painting;
    std::unique_ptr<terrain_to_surface> m_terrain_to_surface;

    void ExpandTerrainToFitTrackSegments();

    int m_max_operations = 0;

    std::optional<std::string> m_track_network;
    std::optional<std::string> m_world_types;

public:
    std::function<void(int)>         m_progress_callback;
    std::function<void(std::string)> m_completion_callback;
    std::function<void(std::string)> m_failure_callback;
    std::function<void(std::string)> m_notification_callback;

    void SetProgressCallback(std::function<void(int)> cb) { m_progress_callback = cb; }
    void SetCompletionCallback(std::function<void(std::string)> cb) { m_completion_callback = cb; }
    void SetFailureCallback(std::function<void(std::string)> cb) { m_failure_callback = cb; }
    void SetNotificationCallback(std::function<void(std::string)> cb) { m_notification_callback = cb; }

    void UpdateUI(std::vector<Unigine::NodePtr> &selection);

};
