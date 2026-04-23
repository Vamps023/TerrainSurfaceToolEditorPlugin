#pragma once

#include "../terrain/TerrainBrushSettings.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QShowEvent>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <UnigineObjects.h>

namespace UnigineEditor
{
class TerrainSurfaceToolEditorPlugin;
}

class TerrainToolPanel : public QWidget
{
    Q_OBJECT

public:
    explicit TerrainToolPanel(UnigineEditor::TerrainSurfaceToolEditorPlugin* plugin);
    ~TerrainToolPanel() override = default;

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void onApplyPullTerrain();
    void onApplyToMask();
    void onPaintCompleteWhite();
    void onRefreshLandscapeTiles();
    void refreshSurfaceOptions();
    void checkSelectionChanged();

private:
    void setupUi();
    void appendLog(const QString& message);
    TerrainBrushSettings currentSettings() const;
    void refreshLandscapeTileOptions(bool preserve_selection = true);
    Unigine::LandscapeLayerMapPtr currentLandscapeTile() const;

    UnigineEditor::TerrainSurfaceToolEditorPlugin* plugin_ = nullptr;

    QComboBox* combo_surface_name_ = nullptr;
    QComboBox* combo_landscape_tile_ = nullptr;
    QPushButton* button_refresh_tiles_ = nullptr;
    QComboBox* combo_mask_name_ = nullptr;
    QDoubleSpinBox* spin_flat_distance_ = nullptr;
    QDoubleSpinBox* spin_falloff_distance_ = nullptr;
    QPushButton* button_pull_ = nullptr;
    QPushButton* button_mask_ = nullptr;
    QPushButton* button_paint_white_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QTextEdit* status_text_ = nullptr;
    QTimer* selection_check_timer_ = nullptr;
    QSet<int> previous_selection_ids_;
};
