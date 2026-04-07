#include "TerrainToolPanel.h"

#include "TerrainSurfaceToolEditorPlugin.h"

#include <QtGui/QPalette>
#include <QHBoxLayout>
#include <QLabel>

using namespace Unigine;

TerrainToolPanel::TerrainToolPanel(UnigineEditor::TerrainSurfaceToolEditorPlugin* plugin)
    : QWidget()
    , plugin_(plugin)
{
    setupUi();

    setWindowTitle("Terrain Surface Tool");
    resize(400, 700);

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

void TerrainToolPanel::setupUi()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setSpacing(10);
    main_layout->setContentsMargins(15, 15, 15, 15);

    auto* header = new QLabel("Terrain Surface Tool", this);
    QFont header_font = header->font();
    header_font.setBold(true);
    header_font.setPointSize(14);
    header->setFont(header_font);
    main_layout->addWidget(header);

    auto* surface_group = new QGroupBox("Surface Filter", this);
    auto* surface_layout = new QHBoxLayout(surface_group);
    surface_layout->addWidget(new QLabel("Name/Regex:", this));
    edit_surface_name_ = new QLineEdit("base", this);
    edit_surface_name_->setToolTip("Surface name or regex pattern to match on selected meshes");
    surface_layout->addWidget(edit_surface_name_);
    main_layout->addWidget(surface_group);

    auto* mask_group = new QGroupBox("Landscape Mask Slot", this);
    auto* mask_layout = new QHBoxLayout(mask_group);
    mask_layout->addWidget(new QLabel("Mask:", this));
    combo_mask_name_ = new QComboBox(this);
    combo_mask_name_->setToolTip(
        "Select the logical landscape mask.\n"
        "Mask 0-3 belong to mask_0 (R/G/B/A), mask 4-7 belong to mask_1, and so on.");
    for (int mask_index = 0; mask_index < 20; ++mask_index)
    {
        const int mask_page = mask_index / 4;
        const QChar channel("RGBA"[mask_index % 4]);
        combo_mask_name_->addItem(
            QString("Mask %1  (mask_%2 / %3)").arg(mask_index).arg(mask_page).arg(channel),
            mask_index);
    }
    mask_layout->addWidget(combo_mask_name_);
    main_layout->addWidget(mask_group);

    auto* brush_group = new QGroupBox("Brush Settings", this);
    auto* brush_layout = new QVBoxLayout(brush_group);

    auto addSpinRow = [&](const QString& label, double min_value, double max_value,
                          double default_value, double step, const QString& tooltip) -> QDoubleSpinBox*
    {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(label, this));
        auto* spin = new QDoubleSpinBox(this);
        spin->setRange(min_value, max_value);
        spin->setValue(default_value);
        spin->setSingleStep(step);
        spin->setToolTip(tooltip);
        row->addWidget(spin);
        brush_layout->addLayout(row);
        return spin;
    };

    spin_brush_size_ = addSpinRow("Brush Size:", 0.1, 1000.0, 10.0, 1.0,
                                  "Brush width in world units");
    spin_flat_distance_ = addSpinRow("Flat Distance:", 0.0, 500.0, 30.0, 5.0,
                                     "Flat sculpt distance in world units");
    spin_falloff_distance_ = addSpinRow("Falloff Distance:", 0.0, 2000.0, 30.0, 5.0,
                                        "Falloff distance in world units");
    main_layout->addWidget(brush_group);

    auto* actions_group = new QGroupBox("Actions", this);
    auto* actions_layout = new QVBoxLayout(actions_group);

    button_pull_ = new QPushButton("Apply Pull Terrain", this);
    button_pull_->setStyleSheet(
        "QPushButton { background-color: #2a7f2a; color: white; padding: 10px; font-weight: bold; }");
    connect(button_pull_, &QPushButton::clicked, this, &TerrainToolPanel::onApplyPullTerrain);
    actions_layout->addWidget(button_pull_);

    button_mask_ = new QPushButton("Apply to Landscape Mask", this);
    button_mask_->setStyleSheet(
        "QPushButton { background-color: #7f2a7f; color: white; padding: 8px; }");
    connect(button_mask_, &QPushButton::clicked, this, &TerrainToolPanel::onApplyToMask);
    actions_layout->addWidget(button_mask_);

    button_reset_ = new QPushButton("Reset / Clear Heights", this);
    button_reset_->setStyleSheet(
        "QPushButton { background-color: #8f2a2a; color: white; padding: 8px; }");
    connect(button_reset_, &QPushButton::clicked, this, &TerrainToolPanel::onResetTerrainHeight);
    actions_layout->addWidget(button_reset_);

    main_layout->addWidget(actions_group);

    progress_bar_ = new QProgressBar(this);
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    main_layout->addWidget(progress_bar_);

    main_layout->addWidget(new QLabel("Status:", this));
    status_text_ = new QTextEdit(this);
    status_text_->setMaximumHeight(150);
    status_text_->setReadOnly(true);
    status_text_->setPlainText("Ready. Select mesh nodes in editor and click Apply.");
    main_layout->addWidget(status_text_);

    main_layout->addStretch();
}

void TerrainToolPanel::appendLog(const QString& message)
{
    if (!status_text_)
        return;

    status_text_->append(message);
    if (auto* scroll_bar = status_text_->verticalScrollBar())
        scroll_bar->setValue(scroll_bar->maximum());

    Log::message("%s\n", message.toUtf8().constData());
}

TerrainBrushSettings TerrainToolPanel::currentSettings() const
{
    TerrainBrushSettings settings;
    settings.brush_size = spin_brush_size_ ? spin_brush_size_->value() : 10.0;
    settings.flat_distance = spin_flat_distance_ ? spin_flat_distance_->value() : 30.0;
    settings.falloff_distance = spin_falloff_distance_ ? spin_falloff_distance_->value() : 30.0;
    settings.smoothing_strength = 0.5;
    return settings;
}

void TerrainToolPanel::onApplyPullTerrain()
{
    appendLog("=== Pull Terrain To Surface ===");
    progress_bar_->setValue(0);

    if (!plugin_ || !plugin_->manipulator())
    {
        appendLog("ERROR: Plugin is not initialized.");
        return;
    }

    const std::vector<NodePtr> selected_nodes = plugin_->getSelectedMeshNodes();
    if (selected_nodes.empty())
    {
        appendLog("ERROR: No mesh nodes selected.");
        return;
    }

    const std::string surface_name = edit_surface_name_->text().toStdString();
    if (surface_name.empty())
    {
        appendLog("ERROR: Surface name is empty.");
        return;
    }

    const TerrainBrushSettings settings = currentSettings();
    appendLog(QString("Found %1 mesh node(s)").arg(selected_nodes.size()));
    progress_bar_->setValue(10);

    const bool queued = plugin_->manipulator()->pullTerrainToSurface(
        selected_nodes,
        surface_name,
        settings,
        [this](const std::string& message)
        {
            appendLog(QString::fromStdString(message));
        });

    progress_bar_->setValue(100);
    appendLog(queued ? "=== Pull Terrain Complete ===" : "=== Pull Terrain Complete (No Changes) ===");
}

void TerrainToolPanel::onApplyToMask()
{
    appendLog("=== Apply to Landscape Mask ===");
    progress_bar_->setValue(0);

    if (!plugin_ || !plugin_->manipulator())
    {
        appendLog("ERROR: Plugin is not initialized.");
        return;
    }

    const std::vector<NodePtr> selected_nodes = plugin_->getSelectedMeshNodes();
    if (selected_nodes.empty())
    {
        appendLog("ERROR: No mesh nodes selected.");
        return;
    }

    const std::string surface_name = edit_surface_name_->text().toStdString();
    if (surface_name.empty())
    {
        appendLog("ERROR: Surface name is empty.");
        return;
    }

    bool ok = false;
    const int mask_index = combo_mask_name_ ? combo_mask_name_->currentData().toInt(&ok) : -1;
    if (!ok || mask_index < 0 || mask_index > 19)
    {
        appendLog("ERROR: Invalid mask slot selected.");
        return;
    }

    const TerrainBrushSettings settings = currentSettings();
    appendLog(QString("Found %1 mesh node(s)").arg(selected_nodes.size()));
    progress_bar_->setValue(10);

    const bool queued = plugin_->manipulator()->applyLandscapeMask(
        selected_nodes,
        surface_name,
        settings,
        mask_index,
        [this](const std::string& message)
        {
            appendLog(QString::fromStdString(message));
        });

    progress_bar_->setValue(100);
    appendLog(queued ? "=== Apply to Landscape Mask Complete ==="
                     : "=== Apply to Landscape Mask Complete (No Changes) ===");
}

void TerrainToolPanel::onResetTerrainHeight()
{
    appendLog("=== Reset Terrain Heights ===");
    progress_bar_->setValue(0);

    if (!plugin_ || !plugin_->manipulator())
    {
        appendLog("ERROR: Plugin is not initialized.");
        return;
    }

    const std::vector<NodePtr> selected_nodes = plugin_->getSelectedMeshNodes();
    if (selected_nodes.empty())
    {
        appendLog("ERROR: No mesh nodes selected.");
        return;
    }

    progress_bar_->setValue(10);
    const bool queued = plugin_->manipulator()->resetTerrainHeights(
        selected_nodes,
        [this](const std::string& message)
        {
            appendLog(QString::fromStdString(message));
        });

    progress_bar_->setValue(100);
    appendLog(queued ? "=== Reset Complete ===" : "=== Reset Complete (No Changes) ===");
}
