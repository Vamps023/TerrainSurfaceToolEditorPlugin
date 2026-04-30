#include "TerrainToolPanel.h"

#include "TerrainToolController.h"

#include <QtGui/QPalette>
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QTimer>
#include <algorithm>

using namespace Unigine;

namespace
{
constexpr int kMaxLandscapeMaskIndex = 19;
}

TerrainToolPanel::TerrainToolPanel(UnigineEditor::TerrainSurfaceToolEditorPlugin* plugin)
    : QWidget()
    , controller(std::make_unique<TerrainToolController>(plugin))
{
    setupUi();
    refreshLandscapeTileOptions(false);

    setWindowTitle("Terrain Surface Tool");
    resize(400, 700);

    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, QColor(45, 45, 45));
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, QColor(35, 35, 35));
    darkPalette.setColor(QPalette::AlternateBase, QColor(45, 45, 45));
    darkPalette.setColor(QPalette::ToolTipBase, Qt::white);
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, QColor(60, 60, 60));
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::BrightText, Qt::red);
    darkPalette.setColor(QPalette::Link, QColor(42, 130, 218));
    darkPalette.setColor(QPalette::Highlight, QColor(42, 130, 218));
    darkPalette.setColor(QPalette::HighlightedText, Qt::black);
    setPalette(darkPalette);
}

void TerrainToolPanel::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(15, 15, 15, 15);

    auto* header = new QLabel("Terrain Surface Tool", this);
    QFont headerFont = header->font();
    headerFont.setBold(true);
    headerFont.setPointSize(14);
    header->setFont(headerFont);
    mainLayout->addWidget(header);

    auto* surfaceGroup = new QGroupBox("Surface Filter", this);
    auto* surfaceLayout = new QHBoxLayout(surfaceGroup);
    surfaceLayout->addWidget(new QLabel("Surface:", this));
    comboSurfaceName = new QComboBox(this);
    comboSurfaceName->setToolTip("Surface name from selected mesh");
    surfaceLayout->addWidget(comboSurfaceName);
    mainLayout->addWidget(surfaceGroup);

    auto* landscapeGroup = new QGroupBox("Landscape Target", this);
    auto* landscapeLayout = new QVBoxLayout(landscapeGroup);
    auto* tileRow = new QHBoxLayout();
    tileRow->addWidget(new QLabel("Tile:", this));
    comboLandscapeTile = new QComboBox(this);
    comboLandscapeTile->setToolTip("Choose a specific LandscapeLayerMap tile, or leave it on All Tiles.");
    tileRow->addWidget(comboLandscapeTile, 1);
    buttonRefreshTiles = new QPushButton(this);
    buttonRefreshTiles->setIcon(QApplication::style()->standardIcon(QStyle::SP_BrowserReload));
    buttonRefreshTiles->setToolTip("Refresh landscape tiles and surface list");
    buttonRefreshTiles->setMaximumWidth(32);
    connect(buttonRefreshTiles, &QPushButton::clicked, this, &TerrainToolPanel::onRefreshLandscapeTiles);
    tileRow->addWidget(buttonRefreshTiles);
    landscapeLayout->addLayout(tileRow);
    mainLayout->addWidget(landscapeGroup);

    auto* maskGroup = new QGroupBox("Landscape Mask Slot", this);
    auto* maskLayout = new QHBoxLayout(maskGroup);
    maskLayout->addWidget(new QLabel("Mask:", this));
    comboMaskName = new QComboBox(this);
    comboMaskName->setToolTip(
        "Select the logical landscape mask.\n"
        "Mask 0-3 belong to mask_0 (R/G/B/A), mask 4-7 belong to mask_1, and so on.");
    for (int maskIndex = 0; maskIndex <= kMaxLandscapeMaskIndex; ++maskIndex)
    {
        const int maskPage = maskIndex / 4;
        const QChar channel("RGBA"[maskIndex % 4]);
        comboMaskName->addItem(
            QString("Mask %1  (mask_%2 / %3)").arg(maskIndex).arg(maskPage).arg(channel),
            maskIndex);
    }
    maskLayout->addWidget(comboMaskName);
    mainLayout->addWidget(maskGroup);

    auto* brushGroup = new QGroupBox("Brush Settings", this);
    auto* brushLayout = new QVBoxLayout(brushGroup);

    auto addSpinRow = [&](const QString& label, double minValue, double maxValue,
                          double defaultValue, double step, const QString& tooltip) -> QDoubleSpinBox*
    {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(label, this));
        auto* spin = new QDoubleSpinBox(this);
        spin->setRange(minValue, maxValue);
        spin->setValue(defaultValue);
        spin->setSingleStep(step);
        spin->setToolTip(tooltip);
        row->addWidget(spin);
        brushLayout->addLayout(row);
        return spin;
    };

    spinFlatDistance = addSpinRow("Flat Distance:", 0.0, 500.0, 5.0, 1.0,
                                     "Padding (in world units) around mesh kept at the exact\n"
                                     "mesh height. Set to 0 to blend from mesh edge directly.");
    spinFalloffDistance = addSpinRow("Falloff Distance:", 0.0, 2000.0, 20.0, 1.0,
                                        "Smooth blend distance (in world units) where terrain\n"
                                        "transitions from the mesh height back to original.");

    mainLayout->addWidget(brushGroup);

    auto* actionsGroup = new QGroupBox("Actions", this);
    auto* actionsLayout = new QVBoxLayout(actionsGroup);

    buttonPull = new QPushButton("Apply Pull Terrain", this);
    buttonPull->setStyleSheet(
        "QPushButton { background-color: #2a7f2a; color: white; padding: 10px; font-weight: bold; }");
    connect(buttonPull, &QPushButton::clicked, this, &TerrainToolPanel::onApplyPullTerrain);
    actionsLayout->addWidget(buttonPull);

    buttonMask = new QPushButton("Apply to Landscape Mask", this);
    buttonMask->setStyleSheet(
        "QPushButton { background-color: #7f2a7f; color: white; padding: 8px; }");
    connect(buttonMask, &QPushButton::clicked, this, &TerrainToolPanel::onApplyToMask);
    actionsLayout->addWidget(buttonMask);

    buttonPaintWhite = new QPushButton("Paint Complete White Height", this);
    buttonPaintWhite->setStyleSheet(
        "QPushButton { background-color: #2a7f8f; color: white; padding: 8px; }");
    connect(buttonPaintWhite, &QPushButton::clicked, this, &TerrainToolPanel::onPaintCompleteWhite);
    actionsLayout->addWidget(buttonPaintWhite);

    mainLayout->addWidget(actionsGroup);

    progressBar = new QProgressBar(this);
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    mainLayout->addWidget(progressBar);

    mainLayout->addWidget(new QLabel("Status:", this));
    statusText = new QTextEdit(this);
    statusText->setMaximumHeight(150);
    statusText->setReadOnly(true);
    statusText->setPlainText("Ready. Select mesh nodes in editor and click Apply.");
    mainLayout->addWidget(statusText);

    mainLayout->addStretch();

    selectionCheckTimer = new QTimer(this);
    selectionCheckTimer->setInterval(500);
    connect(selectionCheckTimer, &QTimer::timeout, this, &TerrainToolPanel::checkSelectionChanged);
    selectionCheckTimer->start();
}

void TerrainToolPanel::showEvent(QShowEvent* event)
{
    refreshLandscapeTileOptions(true);
    refreshSurfaceOptions();
    QWidget::showEvent(event);
}

void TerrainToolPanel::onRefreshLandscapeTiles()
{
    refreshLandscapeTileOptions(true);
    refreshSurfaceOptions();
    appendLog("Landscape tiles and surface list refreshed.");
}

void TerrainToolPanel::refreshSurfaceOptions()
{
    if (!comboSurfaceName || !controller)
        return;

    const QString currentText = comboSurfaceName->currentText();
    comboSurfaceName->blockSignals(true);
    comboSurfaceName->clear();

    const QStringList sortedNames = controller->selectedSurfaceNames();

    for (const QString& name : sortedNames)
        comboSurfaceName->addItem(name);

    if (!currentText.isEmpty() && sortedNames.contains(currentText))
        comboSurfaceName->setCurrentText(currentText);
    else if (comboSurfaceName->count() > 0)
        comboSurfaceName->setCurrentIndex(0);

    comboSurfaceName->blockSignals(false);
}

void TerrainToolPanel::checkSelectionChanged()
{
    if (!controller)
        return;

    if (controller->hasSelectionChanged())
        refreshSurfaceOptions();
}

void TerrainToolPanel::appendLog(const QString& message)
{
    if (!statusText)
        return;

    statusText->append(message);
    if (auto* scrollBar = statusText->verticalScrollBar())
        scrollBar->setValue(scrollBar->maximum());

    Log::message("%s\n", message.toUtf8().constData());
}

TerrainBrushSettings TerrainToolPanel::currentSettings() const
{
    TerrainBrushSettings settings;
    settings.brushSize = 10.0;  // legacy default; only used by mask apply path
    settings.flatDistance = spinFlatDistance ? spinFlatDistance->value() : 30.0;
    settings.falloffDistance = spinFalloffDistance ? spinFalloffDistance->value() : 30.0;
    settings.smoothingStrength = 0.5;
    settings.clampToOriginal = false;
    return settings;
}

void TerrainToolPanel::refreshLandscapeTileOptions(bool preserveSelection)
{
    if (!comboLandscapeTile || !controller)
        return;

    const QVariant previousSelection = preserveSelection ? comboLandscapeTile->currentData() : QVariant();

    comboLandscapeTile->blockSignals(true);
    comboLandscapeTile->clear();
    const QVector<TerrainToolController::TileOption> options = controller->landscapeTileOptions();
    for (const TerrainToolController::TileOption& option : options)
        comboLandscapeTile->addItem(option.label, option.nodeId);

    int selectionIndex = 0;
    if (previousSelection.isValid())
    {
        const int previousIndex = comboLandscapeTile->findData(previousSelection);
        if (previousIndex >= 0)
            selectionIndex = previousIndex;
    }
    comboLandscapeTile->setCurrentIndex(selectionIndex);
    comboLandscapeTile->blockSignals(false);
}

int TerrainToolPanel::currentLandscapeTileId() const
{
    if (!comboLandscapeTile)
        return -1;

    bool ok = false;
    const int tileId = comboLandscapeTile->currentData().toInt(&ok);
    return ok ? tileId : -1;
}

void TerrainToolPanel::onApplyPullTerrain()
{
    refreshLandscapeTileOptions(true);
    appendLog("=== Pull Terrain To Surface ===");
    progressBar->setValue(0);

    const std::string surfaceName = comboSurfaceName->currentText().toStdString();
    const TerrainBrushSettings settings = currentSettings();
    const TerrainToolController::ApplyResult result = controller->pullTerrainToSurface(
        currentLandscapeTileId(),
        surfaceName,
        settings,
        [this](const std::string& message)
        {
            appendLog(QString::fromStdString(message));
        });
    if (!result.error.isEmpty())
    {
        appendLog(result.error);
        return;
    }
    appendLog(QString("Found %1 mesh node(s)").arg(result.selectedMeshCount));
    progressBar->setValue(10);

    progressBar->setValue(100);
    appendLog(result.queued ? "=== Pull Terrain Complete ===" : "=== Pull Terrain Complete (No Changes) ===");
}

void TerrainToolPanel::onApplyToMask()
{
    refreshLandscapeTileOptions(true);
    appendLog("=== Apply to Landscape Mask ===");
    progressBar->setValue(0);

    const std::string surfaceName = comboSurfaceName->currentText().toStdString();
    bool ok = false;
    const int maskIndex = comboMaskName ? comboMaskName->currentData().toInt(&ok) : -1;
    if (!ok || maskIndex < 0 || maskIndex > kMaxLandscapeMaskIndex)
    {
        appendLog("ERROR: Invalid mask slot selected.");
        return;
    }

    const TerrainBrushSettings settings = currentSettings();
    const TerrainToolController::ApplyResult result = controller->applyLandscapeMask(
        currentLandscapeTileId(),
        surfaceName,
        settings,
        maskIndex,
        [this](const std::string& message)
        {
            appendLog(QString::fromStdString(message));
        });
    if (!result.error.isEmpty())
    {
        appendLog(result.error);
        return;
    }
    appendLog(QString("Found %1 mesh node(s)").arg(result.selectedMeshCount));
    progressBar->setValue(10);

    progressBar->setValue(100);
    appendLog(result.queued ? "=== Apply to Landscape Mask Complete ==="
                            : "=== Apply to Landscape Mask Complete (No Changes) ===");
}


void TerrainToolPanel::onPaintCompleteWhite()
{
    appendLog("=== Paint Complete White Height ===");
    progressBar->setValue(0);

    progressBar->setValue(10);
    const TerrainBrushSettings settings = currentSettings();
    const TerrainToolController::ApplyResult result = controller->paintWhiteHeight(
        currentLandscapeTileId(),
        settings,
        [this](const std::string& message)
        {
            appendLog(QString::fromStdString(message));
        });
    if (!result.error.isEmpty())
    {
        appendLog(result.error);
        return;
    }

    appendLog("Erasing selected terrain tile heightmap data.");
    progressBar->setValue(50);
    progressBar->setValue(100);
    appendLog(result.queued ? "=== Paint White Complete ===" : "=== Paint White Complete (No Changes) ===");
}
