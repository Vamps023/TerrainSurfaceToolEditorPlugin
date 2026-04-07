#pragma once

#include "TerrainManipulator.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>

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

private slots:
    void onApplyPullTerrain();
    void onApplyToMask();
    void onResetTerrainHeight();

private:
    void setupUi();
    void appendLog(const QString& message);
    TerrainBrushSettings currentSettings() const;

    UnigineEditor::TerrainSurfaceToolEditorPlugin* plugin_ = nullptr;

    QLineEdit* edit_surface_name_ = nullptr;
    QComboBox* combo_mask_name_ = nullptr;
    QDoubleSpinBox* spin_brush_size_ = nullptr;
    QDoubleSpinBox* spin_flat_distance_ = nullptr;
    QDoubleSpinBox* spin_falloff_distance_ = nullptr;
    QPushButton* button_pull_ = nullptr;
    QPushButton* button_mask_ = nullptr;
    QPushButton* button_reset_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QTextEdit* status_text_ = nullptr;
};
