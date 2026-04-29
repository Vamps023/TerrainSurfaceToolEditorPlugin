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

#include <memory>

namespace UnigineEditor
{
class TerrainSurfaceToolEditorPlugin;
}
class TerrainToolController;

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
    void refreshLandscapeTileOptions(bool preserveSelection = true);
    int currentLandscapeTileId() const;

    std::unique_ptr<TerrainToolController> controller;

    QComboBox* comboSurfaceName = nullptr;
    QComboBox* comboLandscapeTile = nullptr;
    QPushButton* buttonRefreshTiles = nullptr;
    QComboBox* comboMaskName = nullptr;
    QDoubleSpinBox* spinFlatDistance = nullptr;
    QDoubleSpinBox* spinFalloffDistance = nullptr;
    QPushButton* buttonPull = nullptr;
    QPushButton* buttonMask = nullptr;
    QPushButton* buttonPaintWhite = nullptr;
    QProgressBar* progressBar = nullptr;
    QTextEdit* statusText = nullptr;
    QTimer* selectionCheckTimer = nullptr;
    QSet<int> previousSelectionIds;
};
