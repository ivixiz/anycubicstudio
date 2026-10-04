#include "main_window.hpp"

#include "print_view.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDebug>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

QDoubleSpinBox *spin(double minimum, double maximum, double value, int decimals = 3,
                     double step = 1.0) {
    auto *result = new QDoubleSpinBox;
    result->setRange(minimum, maximum);
    result->setDecimals(decimals);
    result->setSingleStep(step);
    result->setValue(value);
    result->setKeyboardTracking(true);
    return result;
}

QWidget *rowOf(std::initializer_list<QWidget *> widgets) {
    auto *container = new QWidget;
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    for (QWidget *widget : widgets)
        layout->addWidget(widget);
    return container;
}

QJsonArray tripleJson(const QVector3D &value) {
    return {value.x(), value.y(), value.z()};
}

bool filenameSuggestsFrontMirror(const QString &path) {
    QString normalized;
    const QString name = QFileInfo(path).fileName().toLower();
    for (const QChar character : name) {
        if (character.isLetterOrNumber())
            normalized.append(character);
        else if (!normalized.endsWith('_'))
            normalized.append('_');
    }
    const QString padded = '_' + normalized + '_';
    for (const QString &marker : {QStringLiteral("f_cu"), QStringLiteral("f_mask"),
                                  QStringLiteral("f_paste"),
                                  QStringLiteral("f_silkscreen")}) {
        if (padded.contains('_' + marker + '_'))
            return true;
    }
    return false;
}

QString typeName(SceneObjectType type) {
    switch (type) {
    case SceneObjectType::Model:
        return "3D";
    case SceneObjectType::Gerber:
        return "Gerber";
    case SceneObjectType::Drill:
        return "Drill";
    case SceneObjectType::ViewerScene:
        return "Scene";
    }
    return "Object";
}

QString sourceTypeJson(SceneObjectType type) {
    switch (type) {
    case SceneObjectType::Gerber:
        return "gerber";
    case SceneObjectType::Drill:
        return "drill";
    default:
        return "model";
    }
}

QJsonObject transformJson(const SceneObject &object) {
    QVector3D scale = object.scale;
    const bool automatic_front = object.isArtwork() && object.pcb.auto_mirror &&
                                 filenameSuggestsFrontMirror(object.path);
    if (object.mirror_x || automatic_front)
        scale.setX(-scale.x());
    if (object.mirror_y)
        scale.setY(-scale.y());
    if (object.mirror_z)
        scale.setZ(-scale.z());
    if (object.isArtwork()) {
        const float uniform = std::abs(scale.x());
        scale.setY(std::copysign(uniform, scale.y()));
        scale.setZ(1.0f);
    }
    return {{"translation_mm", tripleJson(object.move)},
            {"rotation_deg", tripleJson(object.rotate)},
            {"scale", tripleJson(scale)},
            {"auto_center_xy", true},
            {"place_on_bed", true}};
}

QJsonObject supportJson(const SupportSettings &support) {
    QJsonArray manual;
    for (const ManualSupport &point : support.manual) {
        QJsonObject value{{"contact_mm", tripleJson(point.contact)}};
        value.insert("base_z_mm",
                     point.base_z ? QJsonValue(*point.base_z)
                                  : QJsonValue(QJsonValue::Null));
        manual.append(value);
    }
    return {{"enabled", support.enabled},
            {"auto_generate", support.automatic},
            {"shape", support.shape},
            {"overhang_angle_deg", support.angle},
            {"spacing_mm", support.spacing},
            {"tip_diameter_mm", support.tip},
            {"trunk_diameter_mm", support.trunk},
            {"base_diameter_mm", support.base},
            {"tip_height_mm", support.tip_height},
            {"base_height_mm", support.base_height},
            {"penetration_mm", support.penetration},
            {"min_height_mm", support.minimum_height},
            {"max_height_mm", support.maximum_height},
            {"radial_segments", support.segments},
            {"manual_points", manual}};
}

} // namespace

MainWindow::MainWindow(const QStringList &initial_files, QWidget *parent, bool non_interactive)
    : QMainWindow(parent), m_non_interactive(non_interactive) {
    setWindowTitle("Anycubic Studio — unified plate editor");
    resize(1500, 900);
    setAcceptDrops(true);
    buildInterface();
    connectActions();
    loadSettings();
    printingZoneChanged();
    if (!initial_files.isEmpty())
        importPaths(initial_files);
    updateActions();
    if (QFileInfo(m_cloud_config->text()).isFile())
        QTimer::singleShot(0, this, [this] { connectToCloud(false); });
}

void MainWindow::buildInterface() {
    auto *toolbar = addToolBar("Workflow");
    toolbar->setObjectName("workflowToolbar");
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->setMovable(false);
    QAction *import_action = toolbar->addAction("Import…");
    m_layout_action = toolbar->addAction("Auto layout");
    toolbar->addSeparator();
    m_slice_action = toolbar->addAction("Slice…");
    m_run_action = toolbar->addAction("Run…");
    toolbar->addSeparator();
    auto *gizmo_group = new QActionGroup(this);
    gizmo_group->setExclusive(true);
    QAction *move = toolbar->addAction("Move");
    QAction *rotate = toolbar->addAction("Rotate");
    QAction *scale = toolbar->addAction("Scale");
    for (QAction *action : {move, rotate, scale}) {
        action->setCheckable(true);
        gizmo_group->addAction(action);
    }
    move->setChecked(true);
    toolbar->addSeparator();
    m_pause_action = toolbar->addAction("Pause");
    m_resume_action = toolbar->addAction("Resume");
    m_stop_action = toolbar->addAction("Stop");
    m_status_action = toolbar->addAction("Printer status");

    connect(import_action, &QAction::triggered, this, [this] { importDialog(); });
    auto *delete_action = new QAction(this);
    delete_action->setShortcut(QKeySequence::Delete);
    delete_action->setShortcutContext(Qt::WindowShortcut);
    addAction(delete_action);
    connect(delete_action, &QAction::triggered, this, [this] { deleteSelection(); });
    connect(m_layout_action, &QAction::triggered, this, [this] { autoLayout(); });
    connect(m_slice_action, &QAction::triggered, this, [this] { sliceScene(); });
    connect(m_run_action, &QAction::triggered, this, [this] { runPrepared(); });
    connect(move, &QAction::triggered, this,
            [this] { m_view->setGizmoMode(PrintView::GizmoMode::Move); });
    connect(rotate, &QAction::triggered, this,
            [this] { m_view->setGizmoMode(PrintView::GizmoMode::Rotate); });
    connect(scale, &QAction::triggered, this,
            [this] { m_view->setGizmoMode(PrintView::GizmoMode::Scale); });
    connect(m_pause_action, &QAction::triggered, this, [this] { pausePrinting(); });
    connect(m_resume_action, &QAction::triggered, this, [this] { resumePrinting(); });
    connect(m_stop_action, &QAction::triggered, this, [this] { stopPrinting(); });
    connect(m_status_action, &QAction::triggered, this, [this] { requestPrinterStatus(); });

    auto *splitter = new QSplitter;
    auto *object_panel = new QWidget;
    auto *object_layout = new QVBoxLayout(object_panel);
    object_layout->addWidget(new QLabel("Models on plate"));
    m_object_list = new QListWidget;
    m_object_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_object_list->setMinimumWidth(230);
    object_layout->addWidget(m_object_list, 1);
    auto *hint = new QLabel(
        "Drop STEP/STL/3MF/Gerber/DRL together. Click or Ctrl/Shift-click to select; "
        "press Delete to remove selected objects.");
    hint->setWordWrap(true);
    hint->setStyleSheet("color: #8e9bad;");
    object_layout->addWidget(hint);
    splitter->addWidget(object_panel);
    splitter->addWidget(buildCentre());

    m_tabs = new QTabWidget;
    m_tabs->setMinimumWidth(365);
    m_transform_panel = buildTransformPanel();
    m_support_panel = buildSupportPanel();
    m_pcb_panel = buildPcbPanel();
    m_tabs->addTab(m_transform_panel, "Transform");
    m_tabs->addTab(m_support_panel, "Supports");
    m_tabs->addTab(m_pcb_panel, "PCB settings");
    m_tabs->addTab(buildPrintPanel(), "Print & zone");
    m_tabs->addTab(buildCloudPanel(), "Printer/Cloud");
    splitter->addWidget(m_tabs);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    setCentralWidget(splitter);

    auto *dock = new QDockWidget("Rust API output", this);
    dock->setObjectName("rustApiOutputDock");
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    dock->setWidget(m_log);
    dock->setMinimumHeight(120);
    addDockWidget(Qt::BottomDockWidgetArea, dock);

    m_rebuild_timer = new QTimer(this);
    m_rebuild_timer->setSingleShot(true);
    m_rebuild_timer->setInterval(180);
    connect(m_rebuild_timer, &QTimer::timeout, this,
            [this] { rebuildSelectedPreviews(); });
    appendLog("Unified scene ready. Slice prepares a PWSZ; Run only uploads and starts it.");
}

QWidget *MainWindow::buildCentre() {
    auto *container = new QWidget;
    auto *layout = new QVBoxLayout(container);
    auto *controls = new QHBoxLayout;
    m_view_mode = new QComboBox;
    m_view_mode->addItems({"3D objects & supports", "Sliced layers"});
    m_show_model = new QCheckBox("Objects");
    m_show_model->setChecked(true);
    m_show_supports = new QCheckBox("Supports");
    m_show_supports->setChecked(true);
    auto *fit = new QPushButton("Fit view");
    controls->addWidget(m_view_mode);
    controls->addWidget(m_show_model);
    controls->addWidget(m_show_supports);
    controls->addStretch();
    controls->addWidget(fit);
    layout->addLayout(controls);
    m_view = new PrintView;
    layout->addWidget(m_view, 1);
    auto *layers = new QHBoxLayout;
    m_layer_label = new QLabel("No sliced layers");
    m_layer_slider = new QSlider(Qt::Horizontal);
    m_layer_slider->setRange(0, 0);
    m_layer_slider->setEnabled(false);
    layers->addWidget(m_layer_label);
    layers->addWidget(m_layer_slider, 1);
    layout->addLayout(layers);
    auto *help = new QLabel(
        "3D: click object to select; drag selected object with the active Move/Rotate/Scale "
        "gizmo; drag empty space to orbit; middle/right/Shift-drag pans. Layers: wheel zooms "
        "and any drag pans.");
    help->setWordWrap(true);
    help->setStyleSheet("color: #8e9bad;");
    layout->addWidget(help);

    connect(fit, &QPushButton::clicked, this, [this] {
        if (m_view_mode->currentIndex() == 1)
            m_view->fitLayer();
        else
            m_view->resetCamera();
    });
    connect(m_view_mode, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_view->setLayerMode(index == 1);
        m_layer_slider->setEnabled(index == 1 && m_view->layerCount() > 0);
    });
    connect(m_show_model, &QCheckBox::toggled, m_view, &PrintView::setShowModel);
    connect(m_show_supports, &QCheckBox::toggled, m_view, &PrintView::setShowSupports);
    connect(m_layer_slider, &QSlider::valueChanged, this, [this](int value) {
        m_view->setLayer(value);
        updateLayerLabel(value);
    });
    return container;
}

QWidget *MainWindow::buildTransformPanel() {
    auto *panel = new QWidget;
    auto *layout = new QVBoxLayout(panel);
    auto *form = new QFormLayout;
    for (int axis = 0; axis < 3; ++axis) {
        m_move[axis] = spin(-1000.0, 1000.0, 0.0);
        m_rotate[axis] = spin(-360.0, 360.0, 0.0, 2, 5.0);
        m_scale[axis] = spin(0.001, 1000.0, 1.0, 4, 0.05);
        m_mirror[axis] = new QCheckBox(QString(QChar('X' + axis)));
    }
    form->addRow("Move X/Y/Z (mm)", rowOf({m_move[0], m_move[1], m_move[2]}));
    form->addRow("Rotate X/Y/Z (°)", rowOf({m_rotate[0], m_rotate[1], m_rotate[2]}));
    form->addRow("Scale X/Y/Z", rowOf({m_scale[0], m_scale[1], m_scale[2]}));
    form->addRow("Mirror", rowOf({m_mirror[0], m_mirror[1], m_mirror[2]}));
    layout->addLayout(form);
    auto *centre = new QPushButton("Center X/Y");
    auto *bed = new QPushButton("Place on bed");
    auto *reset = new QPushButton("Reset transform");
    layout->addWidget(rowOf({centre, bed}));
    layout->addWidget(reset);
    auto *note = new QLabel(
        "Values are live and apply to all selected objects. Gerber/DRL use Z rotation and "
        "uniform XY scale; a detected KiCad F_* layer starts with Mirror X active.");
    note->setWordWrap(true);
    note->setStyleSheet("color: #8e9bad;");
    layout->addWidget(note);
    layout->addStretch();
    for (int axis = 0; axis < 3; ++axis) {
        connect(m_move[axis], &QDoubleSpinBox::valueChanged, this,
                [this] { applyTransformLive(); });
        connect(m_rotate[axis], &QDoubleSpinBox::valueChanged, this,
                [this] { applyTransformLive(); });
        connect(m_scale[axis], &QDoubleSpinBox::valueChanged, this,
                [this] { applyTransformLive(); });
        connect(m_mirror[axis], &QCheckBox::toggled, this,
                [this] { applyTransformLive(); });
    }
    connect(centre, &QPushButton::clicked, this, [this] { centreSelection(); });
    connect(bed, &QPushButton::clicked, this, [this] { placeSelectionOnBed(); });
    connect(reset, &QPushButton::clicked, this, [this] { resetTransform(); });
    return panel;
}

QWidget *MainWindow::buildSupportPanel() {
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    auto *form = new QFormLayout;
    m_support_preset = new QComboBox;
    m_support_preset->addItems({"Custom", "None", "Light", "Medium", "Heavy"});
    m_support_enabled = new QCheckBox("Generate supports");
    m_support_auto = new QCheckBox("Automatic placement");
    m_support_shape = new QComboBox;
    m_support_shape->addItems({"tapered", "cylinder", "square"});
    m_support_angle = spin(1.1, 88.9, 45.0, 1, 1.0);
    m_support_spacing = spin(0.1, 100.0, 4.0);
    m_support_tip = spin(0.05, 20.0, 0.5);
    m_support_trunk = spin(0.05, 30.0, 1.2);
    m_support_base = spin(0.05, 50.0, 4.5);
    m_support_tip_height = spin(0.01, 50.0, 1.5);
    m_support_base_height = spin(0.01, 50.0, 0.8);
    m_support_penetration = spin(0.0, 10.0, 0.15);
    m_support_min_height = spin(0.0, 500.0, 2.0);
    m_support_max_height = spin(0.01, 1000.0, 180.0);
    m_support_segments = new QSpinBox;
    m_support_segments->setRange(6, 128);
    form->addRow("Preset", m_support_preset);
    form->addRow("Enabled", m_support_enabled);
    form->addRow("Placement", m_support_auto);
    form->addRow("Shape", m_support_shape);
    form->addRow("Overhang angle (°)", m_support_angle);
    form->addRow("Spacing (mm)", m_support_spacing);
    form->addRow("Tip diameter (mm)", m_support_tip);
    form->addRow("Trunk diameter (mm)", m_support_trunk);
    form->addRow("Base diameter (mm)", m_support_base);
    form->addRow("Tip height (mm)", m_support_tip_height);
    form->addRow("Base height (mm)", m_support_base_height);
    form->addRow("Penetration (mm)", m_support_penetration);
    form->addRow("Min height (mm)", m_support_min_height);
    form->addRow("Max height (mm)", m_support_max_height);
    form->addRow("Radial segments", m_support_segments);
    layout->addLayout(form);
    layout->addWidget(new QLabel("Manual contact points"));
    m_manual_supports = new QListWidget;
    layout->addWidget(m_manual_supports);
    auto *add = new QPushButton("Add X,Y,Z[,BASE_Z]");
    auto *remove = new QPushButton("Remove point");
    layout->addWidget(rowOf({add, remove}));
    auto *note = new QLabel(
        "Supports are object state. Slice generates them automatically; no Apply step is needed.");
    note->setWordWrap(true);
    note->setStyleSheet("color: #8e9bad;");
    layout->addWidget(note);
    layout->addStretch();

    connect(m_support_preset, &QComboBox::activated, this,
            [this](int index) { applySupportPreset(index); });
    connect(add, &QPushButton::clicked, this, [this] { addManualSupport(); });
    connect(remove, &QPushButton::clicked, this, [this] { removeManualSupport(); });
    for (QCheckBox *box : {m_support_enabled, m_support_auto})
        connect(box, &QCheckBox::toggled, this, [this] { applySupportsLive(); });
    connect(m_support_shape, &QComboBox::currentIndexChanged, this,
            [this] { applySupportsLive(); });
    for (QDoubleSpinBox *value : {m_support_angle, m_support_spacing, m_support_tip,
                                  m_support_trunk, m_support_base, m_support_tip_height,
                                  m_support_base_height, m_support_penetration,
                                  m_support_min_height, m_support_max_height})
        connect(value, &QDoubleSpinBox::valueChanged, this,
                [this] { applySupportsLive(); });
    connect(m_support_segments, &QSpinBox::valueChanged, this,
            [this] { applySupportsLive(); });
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    return scroll;
}

QWidget *MainWindow::buildPcbPanel() {
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    auto *form = new QFormLayout;
    m_pcb_exposure = spin(0.01, 3600.0, 60.0, 2, 1.0);
    m_pcb_drill_mode = new QComboBox;
    m_pcb_drill_mode->addItems({"clear", "expose", "ignore"});
    m_pcb_invert = new QCheckBox("Invert inside artwork bounds");
    m_pcb_invert_margin = spin(0.0, 1000.0, 0.0);
    m_pcb_simplify = spin(0.0, 10.0, 0.012, 4, 0.002);
    form->addRow("Exposure (s)", m_pcb_exposure);
    form->addRow("Drill pixels", m_pcb_drill_mode);
    form->addRow("Polarity", m_pcb_invert);
    form->addRow("Invert margin (mm)", m_pcb_invert_margin);
    form->addRow("Contour simplify (mm)", m_pcb_simplify);
    layout->addLayout(form);
    m_pcb_status = new QLabel;
    m_pcb_status->setWordWrap(true);
    layout->addWidget(m_pcb_status);
    auto *note = new QLabel(
        "Move/Rotate/Mirror/Scale are in the common Transform tab. Exact LCD output appears "
        "automatically in Sliced layers after Slice; Run uses that prepared PWSZ.");
    note->setWordWrap(true);
    note->setStyleSheet("color: #8e9bad;");
    layout->addWidget(note);
    layout->addStretch();
    connect(m_pcb_exposure, &QDoubleSpinBox::valueChanged, this,
            [this] { applyPcbLive(); });
    connect(m_pcb_drill_mode, &QComboBox::currentIndexChanged, this,
            [this] { applyPcbLive(); });
    connect(m_pcb_invert, &QCheckBox::toggled, this, [this] { applyPcbLive(); });
    connect(m_pcb_invert_margin, &QDoubleSpinBox::valueChanged, this,
            [this] { applyPcbLive(); });
    connect(m_pcb_simplify, &QDoubleSpinBox::valueChanged, this,
            [this] { applyPcbLive(); });
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    return scroll;
}

QWidget *MainWindow::buildPrintPanel() {
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    auto *zone = new QGroupBox("Printing zone — Photon Mono M7 Pro");
    auto *zone_form = new QFormLayout(zone);
    m_zone[0] = spin(1.0, 2000.0, M7_BUILD_X);
    m_zone[1] = spin(1.0, 2000.0, M7_BUILD_Y);
    m_zone[2] = spin(1.0, 2000.0, M7_BUILD_Z);
    zone_form->addRow("X/Y/Z (mm)", rowOf({m_zone[0], m_zone[1], m_zone[2]}));
    zone_form->addRow(new QLabel("13312 × 5120 pixels; 16.8 × 24.8 µm."));
    layout->addWidget(zone);
    auto *print = new QGroupBox("Exposure and motion");
    auto *form = new QFormLayout(print);
    m_layer_height = spin(0.001, 1.0, 0.05, 3, 0.01);
    m_bottom_layers = new QSpinBox;
    m_bottom_layers->setRange(0, 1000);
    m_bottom_exposure = spin(0.0, 1000.0, 28.0, 2, 1.0);
    m_exposure = spin(0.0, 1000.0, 1.8, 2, 0.1);
    m_lift_height = spin(0.0, 100.0, 8.0);
    m_lift_speed = spin(0.001, 1000.0, 6.0);
    m_off_time = spin(0.0, 1000.0, 0.5);
    form->addRow("Layer height (mm)", m_layer_height);
    form->addRow("Bottom layers", m_bottom_layers);
    form->addRow("Bottom exposure (s)", m_bottom_exposure);
    form->addRow("Normal exposure (s)", m_exposure);
    form->addRow("Lift height (mm)", m_lift_height);
    form->addRow("Lift speed (mm/s)", m_lift_speed);
    form->addRow("Off time (s)", m_off_time);
    layout->addWidget(print);
    m_output_label = new QLabel("No prepared PWSZ. Press Slice.");
    m_output_label->setWordWrap(true);
    layout->addWidget(m_output_label);
    auto *note = new QLabel(
        "Any scene/settings change invalidates the prepared file. Run never slices implicitly.");
    note->setWordWrap(true);
    note->setStyleSheet("color: #8e9bad;");
    layout->addWidget(note);
    layout->addStretch();
    for (QDoubleSpinBox *value : m_zone)
        connect(value, &QDoubleSpinBox::valueChanged, this,
                [this] { printingZoneChanged(); });
    for (QDoubleSpinBox *value : {m_layer_height, m_bottom_exposure, m_exposure,
                                  m_lift_height, m_lift_speed, m_off_time})
        connect(value, &QDoubleSpinBox::valueChanged, this,
                [this] { invalidateSlice(); });
    connect(m_bottom_layers, &QSpinBox::valueChanged, this,
            [this] { invalidateSlice(); });
    return content;
}

QWidget *MainWindow::buildCloudPanel() {
    auto *panel = new QWidget;
    auto *layout = new QVBoxLayout(panel);
    auto *form = new QFormLayout;
    m_cloud_config = new QLineEdit;
    m_cloud_config->setPlaceholderText("Anycubic global_config.ini");
    auto *browse = new QPushButton("Browse…");
    form->addRow("Account config", rowOf({m_cloud_config, browse}));
    auto *login = new QPushButton("Log in to Anycubic…");
    form->addRow("OAuth account", login);
    auto *connect_cloud = new QPushButton("Connect / detect printer");
    form->addRow("Cloud", connect_cloud);
    m_printer_id = new QLineEdit;
    form->addRow("Printer ID", m_printer_id);
    layout->addLayout(form);
    m_cloud_file_label = new QLabel("Cloud file: not uploaded");
    layout->addWidget(m_cloud_file_label);
    auto *warning = new QLabel(
        "Run/Pause/Resume/Stop control real hardware. Run requires confirmation. PCB jobs "
        "still use firmware's normal print-start path and may home Z.");
    warning->setWordWrap(true);
    warning->setStyleSheet("color: #d27b68;");
    layout->addWidget(warning);
    auto *exposure = new QGroupBox("Built-in LCD service test (no print/Z command)");
    auto *exposure_form = new QFormLayout(exposure);
    m_exposure_pattern = new QComboBox;
    m_exposure_pattern->addItem("Pattern 1", 1);
    m_exposure_pattern->addItem("Pattern 2", 2);
    m_exposure_pattern->addItem("Pattern 3", 4);
    m_exposure_duration = new QSpinBox;
    m_exposure_duration->setRange(1, 300);
    m_exposure_duration->setSuffix(" s");
    auto *start = new QPushButton("Run built-in LCD test…");
    auto *stop = new QPushButton("STOP LCD test");
    exposure_form->addRow("Pattern", m_exposure_pattern);
    exposure_form->addRow("Duration", m_exposure_duration);
    exposure_form->addRow(rowOf({start, stop}));
    layout->addWidget(exposure);
    layout->addStretch();
    connect(browse, &QPushButton::clicked, this, [this] { browseCloudConfig(); });
    connect(login, &QPushButton::clicked, this, [this] { loginToCloud(); });
    connect(connect_cloud, &QPushButton::clicked, this,
            [this] { connectToCloud(true); });
    connect(start, &QPushButton::clicked, this, [this] { startBuiltinExposure(); });
    connect(stop, &QPushButton::clicked, this, [this] { stopBuiltinExposure(); });
    return panel;
}

void MainWindow::connectActions() {
    connect(m_object_list, &QListWidget::itemSelectionChanged, this,
            [this] { selectionFromList(); });
    m_view->setSelectionHandler(
        [this](int index, Qt::KeyboardModifiers modifiers) {
            selectionFromView(index, modifiers);
        });
    m_view->setTransformHandler([this](PrintView::GizmoMode mode, QVector3D delta) {
        gizmoTransform(static_cast<int>(mode), delta);
    });
}

void MainWindow::loadSettings() {
    QSettings settings;
    const QByteArray geometry = settings.value("window/geometry").toByteArray();
    const QByteArray state = settings.value("window/state").toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    if (!state.isEmpty())
        restoreState(state);
    m_view_mode->setCurrentIndex(settings.value("view/mode", 0).toInt());
    m_show_model->setChecked(settings.value("view/showModel", true).toBool());
    m_show_supports->setChecked(settings.value("view/showSupports", true).toBool());
    m_zone[0]->setValue(settings.value("print/zoneX", M7_BUILD_X).toDouble());
    m_zone[1]->setValue(settings.value("print/zoneY", M7_BUILD_Y).toDouble());
    m_zone[2]->setValue(settings.value("print/zoneZ", M7_BUILD_Z).toDouble());
    m_layer_height->setValue(settings.value("print/layerHeight", 0.05).toDouble());
    m_bottom_layers->setValue(settings.value("print/bottomLayers", 5).toInt());
    m_bottom_exposure->setValue(settings.value("print/bottomExposure", 28.0).toDouble());
    m_exposure->setValue(settings.value("print/exposure", 1.8).toDouble());
    m_lift_height->setValue(settings.value("print/liftHeight", 8.0).toDouble());
    m_lift_speed->setValue(settings.value("print/liftSpeed", 6.0).toDouble());
    m_off_time->setValue(settings.value("print/offTime", 0.5).toDouble());
    m_default_supports.enabled = settings.value("supports/enabled", false).toBool();
    m_default_supports.automatic = settings.value("supports/automatic", true).toBool();
    m_default_supports.shape = settings.value("supports/shape", "tapered").toString();
    m_default_supports.angle = settings.value("supports/angle", 45.0).toDouble();
    m_default_supports.spacing = settings.value("supports/spacing", 4.0).toDouble();
    m_default_supports.tip = settings.value("supports/tip", 0.5).toDouble();
    m_default_supports.trunk = settings.value("supports/trunk", 1.2).toDouble();
    m_default_supports.base = settings.value("supports/base", 4.5).toDouble();
    m_default_supports.tip_height = settings.value("supports/tipHeight", 1.5).toDouble();
    m_default_supports.base_height = settings.value("supports/baseHeight", 0.8).toDouble();
    m_default_supports.penetration = settings.value("supports/penetration", 0.15).toDouble();
    m_default_supports.minimum_height = settings.value("supports/minHeight", 2.0).toDouble();
    m_default_supports.maximum_height = settings.value("supports/maxHeight", 180.0).toDouble();
    m_default_supports.segments = settings.value("supports/segments", 12).toInt();
    m_default_pcb.exposure = settings.value("pcb/exposure", 60.0).toDouble();
    m_default_pcb.drill_mode = settings.value("pcb/drillMode", "clear").toString();
    m_default_pcb.invert = settings.value("pcb/invert", false).toBool();
    m_default_pcb.auto_mirror = settings.value("pcb/autoMirror", true).toBool();
    m_default_pcb.invert_margin = settings.value("pcb/invertMargin", 0.0).toDouble();
    m_default_pcb.simplify = settings.value("pcb/simplify", 0.012).toDouble();
    const QString environment_config = qEnvironmentVariable("ANYCUBIC_CONFIG").trimmed();
    if (!environment_config.isEmpty())
        m_cloud_config->setText(environment_config);
    const QString configured = settings.value("cloud/configPath").toString();
    if (m_cloud_config->text().isEmpty() && !configured.isEmpty())
        m_cloud_config->setText(configured);
    if (m_cloud_config->text().isEmpty()) {
        for (const QString &candidate : {
                 QDir::current().filePath("global_config.ini"),
                 QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
                     "../../global_config.ini")}) {
            if (QFileInfo(candidate).isFile()) {
                m_cloud_config->setText(QFileInfo(candidate).absoluteFilePath());
                break;
            }
        }
    }
    m_printer_id->setText(settings.value("cloud/printerId").toString());
    m_exposure_pattern->setCurrentIndex(settings.value("cloud/exposurePattern", 0).toInt());
    m_exposure_duration->setValue(settings.value("cloud/exposureDuration", 5).toInt());
}

void MainWindow::saveSettings() const {
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState());
    settings.setValue("view/mode", m_view_mode->currentIndex());
    settings.setValue("view/showModel", m_show_model->isChecked());
    settings.setValue("view/showSupports", m_show_supports->isChecked());
    settings.setValue("print/zoneX", m_zone[0]->value());
    settings.setValue("print/zoneY", m_zone[1]->value());
    settings.setValue("print/zoneZ", m_zone[2]->value());
    settings.setValue("print/layerHeight", m_layer_height->value());
    settings.setValue("print/bottomLayers", m_bottom_layers->value());
    settings.setValue("print/bottomExposure", m_bottom_exposure->value());
    settings.setValue("print/exposure", m_exposure->value());
    settings.setValue("print/liftHeight", m_lift_height->value());
    settings.setValue("print/liftSpeed", m_lift_speed->value());
    settings.setValue("print/offTime", m_off_time->value());
    settings.setValue("supports/enabled", m_default_supports.enabled);
    settings.setValue("supports/automatic", m_default_supports.automatic);
    settings.setValue("supports/shape", m_default_supports.shape);
    settings.setValue("supports/angle", m_default_supports.angle);
    settings.setValue("supports/spacing", m_default_supports.spacing);
    settings.setValue("supports/tip", m_default_supports.tip);
    settings.setValue("supports/trunk", m_default_supports.trunk);
    settings.setValue("supports/base", m_default_supports.base);
    settings.setValue("supports/tipHeight", m_default_supports.tip_height);
    settings.setValue("supports/baseHeight", m_default_supports.base_height);
    settings.setValue("supports/penetration", m_default_supports.penetration);
    settings.setValue("supports/minHeight", m_default_supports.minimum_height);
    settings.setValue("supports/maxHeight", m_default_supports.maximum_height);
    settings.setValue("supports/segments", m_default_supports.segments);
    settings.setValue("pcb/exposure", m_default_pcb.exposure);
    settings.setValue("pcb/drillMode", m_default_pcb.drill_mode);
    settings.setValue("pcb/invert", m_default_pcb.invert);
    settings.setValue("pcb/autoMirror", m_default_pcb.auto_mirror);
    settings.setValue("pcb/invertMargin", m_default_pcb.invert_margin);
    settings.setValue("pcb/simplify", m_default_pcb.simplify);
    settings.setValue("cloud/configPath", m_cloud_config->text().trimmed());
    settings.setValue("cloud/printerId", m_printer_id->text().trimmed());
    settings.setValue("cloud/exposurePattern", m_exposure_pattern->currentIndex());
    settings.setValue("cloud/exposureDuration", m_exposure_duration->value());
    settings.sync();
}

void MainWindow::closeEvent(QCloseEvent *event) {
    saveSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (!event->mimeData()->hasUrls())
        return;
    for (const QUrl &url : event->mimeData()->urls()) {
        const QString path = url.toLocalFile();
        if (supportedModel(path) || supportedArtwork(path) ||
            QFileInfo(path).suffix().compare("acv", Qt::CaseInsensitive) == 0) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent *event) {
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls())
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    importPaths(paths);
    event->acceptProposedAction();
}

void MainWindow::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Delete) {
        deleteSelection();
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::importDialog() {
    importPaths(QFileDialog::getOpenFileNames(
        this, "Import objects", QString(),
        "Supported (*.stl *.3mf *.step *.stp *.iges *.igs *.gbr *.ger *.pho *.gtl *.gbl "
        "*.gts *.gbs *.gto *.gbo *.gm1 *.gko *.drl *.xln *.exc);;All files (*)"));
}

void MainWindow::importPaths(const QStringList &paths) {
    int imported = 0;
    for (const QString &path : paths)
        imported += importPath(path) ? 1 : 0;
    if (imported == 0)
        return;
    for (auto &object : m_objects)
        object->selected = false;
    m_objects.back()->selected = true;
    refreshObjectList();
    refreshView();
    refreshPropertyPanels();
    invalidateSlice();
}

bool MainWindow::importPath(const QString &source) {
    const QFileInfo info(source);
    if (!info.isFile()) {
        const QString message = "File does not exist:\n" + source;
        if (m_non_interactive)
            appendLog(message);
        else
            QMessageBox::warning(this, "Import", message);
        return false;
    }
    auto object = std::make_unique<SceneObject>();
    object->id = m_next_object_id++;
    object->path = info.absoluteFilePath();
    object->display_name = info.fileName();
    object->supports = m_default_supports;
    object->pcb = m_default_pcb;
    if (supportedModel(object->path))
        object->type = SceneObjectType::Model;
    else if (supportedGerber(object->path))
        object->type = SceneObjectType::Gerber;
    else if (supportedDrill(object->path))
        object->type = SceneObjectType::Drill;
    else if (info.suffix().compare("acv", Qt::CaseInsensitive) == 0) {
        object->type = SceneObjectType::ViewerScene;
        object->editable = false;
        QString error;
        if (!loadScene(object->path, object->visual, error)) {
            if (m_non_interactive)
                appendLog(error);
            else
                QMessageBox::critical(this, "Import", error);
            return false;
        }
    } else {
        const QString message = "Unsupported format:\n" + object->path;
        if (m_non_interactive)
            appendLog(message);
        else
            QMessageBox::warning(this, "Import", message);
        return false;
    }
    if (object->type == SceneObjectType::Gerber && object->pcb.auto_mirror &&
        filenameSuggestsFrontMirror(object->path)) {
        appendLog("Auto-mirror X enabled for front-side KiCad layer " + object->display_name);
    }
    if (object->editable && !rebuildObject(*object, !m_non_interactive)) {
        appendLog("Import failed for " + object->path);
        return false;
    }
    appendLog("Imported " + typeName(object->type) + " object " + object->path);
    m_objects.push_back(std::move(object));
    return true;
}

std::vector<SceneObject *> MainWindow::selectedObjects() {
    std::vector<SceneObject *> result;
    for (auto &object : m_objects)
        if (object->selected)
            result.push_back(object.get());
    return result;
}

std::vector<const SceneObject *> MainWindow::selectedObjects() const {
    std::vector<const SceneObject *> result;
    for (const auto &object : m_objects)
        if (object->selected)
            result.push_back(object.get());
    return result;
}

SceneObject *MainWindow::firstSelectedModel() {
    for (SceneObject *object : selectedObjects())
        if (object->isModel())
            return object;
    return nullptr;
}

SceneObject *MainWindow::firstSelectedArtwork() {
    for (SceneObject *object : selectedObjects())
        if (object->isArtwork())
            return object;
    return nullptr;
}

void MainWindow::selectionFromList() {
    if (m_loading_controls)
        return;
    for (int index = 0; index < static_cast<int>(m_objects.size()); ++index)
        m_objects[index]->selected = m_object_list->item(index)->isSelected();
    refreshView();
    refreshPropertyPanels();
    updateActions();
}

void MainWindow::selectionFromView(int index, Qt::KeyboardModifiers modifiers) {
    const bool extend = modifiers & (Qt::ControlModifier | Qt::ShiftModifier);
    if (!extend)
        for (auto &object : m_objects)
            object->selected = false;
    if (index >= 0 && index < static_cast<int>(m_objects.size())) {
        if (extend)
            m_objects[index]->selected = !m_objects[index]->selected;
        else
            m_objects[index]->selected = true;
    }
    refreshObjectList();
    refreshView();
    refreshPropertyPanels();
}

void MainWindow::refreshObjectList() {
    m_loading_controls = true;
    m_object_list->clear();
    for (const auto &object : m_objects) {
        auto *item = new QListWidgetItem(
            QString("[%1] %2").arg(typeName(object->type), object->display_name));
        item->setSelected(object->selected);
        item->setToolTip(object->path);
        m_object_list->addItem(item);
    }
    m_loading_controls = false;
}

void MainWindow::refreshView() {
    m_view->setObjects(m_objects);
    m_view->setLayers(m_sliced_layers);
    updateLayerControls();
}

void MainWindow::refreshPropertyPanels() {
    const auto selected = selectedObjects();
    const bool any = !selected.empty();
    const bool models = std::any_of(selected.begin(), selected.end(),
                                    [](const SceneObject *object) { return object->isModel(); });
    const bool artwork = std::any_of(selected.begin(), selected.end(),
                                     [](const SceneObject *object) { return object->isArtwork(); });
    m_tabs->setTabVisible(0, any);
    m_tabs->setTabVisible(1, models);
    m_tabs->setTabVisible(2, artwork);
    loadTransformControls();
    loadSupportControls();
    loadPcbControls();
}

void MainWindow::loadTransformControls() {
    const auto selected = selectedObjects();
    const SceneObject *object = selected.empty() ? nullptr : selected.front();
    m_loading_controls = true;
    for (int axis = 0; axis < 3; ++axis) {
        m_move[axis]->setEnabled(object && object->editable);
        m_rotate[axis]->setEnabled(object && object->editable);
        m_scale[axis]->setEnabled(object && object->editable);
        m_mirror[axis]->setEnabled(object && object->editable);
        m_move[axis]->setValue(object ? object->move[axis] : 0.0);
        m_rotate[axis]->setValue(object ? object->rotate[axis] : 0.0);
        m_scale[axis]->setValue(object ? object->scale[axis] : 1.0);
        bool mirror = false;
        if (object) {
            mirror = axis == 0   ? object->mirror_x ||
                                      (object->isArtwork() && object->pcb.auto_mirror &&
                                       filenameSuggestsFrontMirror(object->path))
                     : axis == 1 ? object->mirror_y
                                 : object->mirror_z;
        }
        m_mirror[axis]->setChecked(mirror);
    }
    const bool artwork_only = object && std::all_of(selected.begin(), selected.end(),
                                                    [](const SceneObject *value) {
                                                        return value->isArtwork();
                                                    });
    if (artwork_only) {
        m_rotate[0]->setEnabled(false);
        m_rotate[1]->setEnabled(false);
        m_scale[1]->setEnabled(false);
        m_scale[2]->setEnabled(false);
        m_mirror[2]->setEnabled(false);
    }
    m_loading_controls = false;
}

void MainWindow::loadSupportControls() {
    SceneObject *model = firstSelectedModel();
    m_loading_controls = true;
    const QList<QWidget *> widgets{m_support_preset,      m_support_enabled,
                                   m_support_auto,        m_support_shape,
                                   m_support_angle,       m_support_spacing,
                                   m_support_tip,         m_support_trunk,
                                   m_support_base,        m_support_tip_height,
                                   m_support_base_height, m_support_penetration,
                                   m_support_min_height,  m_support_max_height,
                                   m_support_segments,    m_manual_supports};
    for (QWidget *widget : widgets)
        widget->setEnabled(model != nullptr);
    if (model) {
        const SupportSettings &support = model->supports;
        m_support_preset->setCurrentIndex(0);
        m_support_enabled->setChecked(support.enabled);
        m_support_auto->setChecked(support.automatic);
        m_support_shape->setCurrentText(support.shape);
        m_support_angle->setValue(support.angle);
        m_support_spacing->setValue(support.spacing);
        m_support_tip->setValue(support.tip);
        m_support_trunk->setValue(support.trunk);
        m_support_base->setValue(support.base);
        m_support_tip_height->setValue(support.tip_height);
        m_support_base_height->setValue(support.base_height);
        m_support_penetration->setValue(support.penetration);
        m_support_min_height->setValue(support.minimum_height);
        m_support_max_height->setValue(support.maximum_height);
        m_support_segments->setValue(support.segments);
    }
    refreshManualSupports();
    m_loading_controls = false;
}

void MainWindow::loadPcbControls() {
    SceneObject *object = firstSelectedArtwork();
    m_loading_controls = true;
    const QList<QWidget *> pcb_widgets{m_pcb_exposure, m_pcb_drill_mode, m_pcb_invert,
                                       m_pcb_invert_margin, m_pcb_simplify};
    for (QWidget *widget : pcb_widgets)
        widget->setEnabled(object != nullptr);
    if (object) {
        m_pcb_exposure->setValue(object->pcb.exposure);
        m_pcb_drill_mode->setCurrentText(object->pcb.drill_mode);
        m_pcb_invert->setChecked(object->pcb.invert);
        m_pcb_invert_margin->setValue(object->pcb.invert_margin);
        m_pcb_simplify->setValue(object->pcb.simplify);
        const auto selected = selectedObjects();
        const int count = std::count_if(selected.begin(), selected.end(),
                                        [](const SceneObject *value) { return value->isArtwork(); });
        m_pcb_status->setText(QString("%1 PCB object(s) selected. Settings affect only them.")
                                  .arg(count));
    } else {
        m_pcb_status->setText("Select a Gerber/DRL object to edit PCB settings.");
    }
    m_loading_controls = false;
}

void MainWindow::applyTransformLive() {
    if (m_loading_controls)
        return;
    for (SceneObject *object : selectedObjects()) {
        if (!object->editable)
            continue;
        object->move = QVector3D(m_move[0]->value(), m_move[1]->value(), m_move[2]->value());
        object->rotate =
            QVector3D(object->isArtwork() ? 0.0 : m_rotate[0]->value(),
                      object->isArtwork() ? 0.0 : m_rotate[1]->value(),
                      m_rotate[2]->value());
        if (object->isArtwork()) {
            const float uniform = m_scale[0]->value();
            object->scale = QVector3D(uniform, uniform, 1.0f);
        } else {
            object->scale = QVector3D(m_scale[0]->value(), m_scale[1]->value(),
                                      m_scale[2]->value());
        }
        object->mirror_x = m_mirror[0]->isChecked();
        object->mirror_y = m_mirror[1]->isChecked();
        object->mirror_z = object->isModel() && m_mirror[2]->isChecked();
        if (object->isArtwork())
            object->pcb.auto_mirror = false;
    }
    invalidateSlice();
    scheduleRebuild();
}

void MainWindow::applySupportsLive() {
    if (m_loading_controls)
        return;
    for (SceneObject *object : selectedObjects()) {
        if (!object->isModel())
            continue;
        SupportSettings &support = object->supports;
        support.enabled = m_support_enabled->isChecked();
        support.automatic = m_support_auto->isChecked();
        support.shape = m_support_shape->currentText();
        support.angle = m_support_angle->value();
        support.spacing = m_support_spacing->value();
        support.tip = m_support_tip->value();
        support.trunk = m_support_trunk->value();
        support.base = m_support_base->value();
        support.tip_height = m_support_tip_height->value();
        support.base_height = m_support_base_height->value();
        support.penetration = m_support_penetration->value();
        support.minimum_height = m_support_min_height->value();
        support.maximum_height = m_support_max_height->value();
        support.segments = m_support_segments->value();
        m_default_supports = support;
        m_default_supports.manual.clear();
    }
    m_support_preset->setCurrentIndex(0);
    invalidateSlice();
}

void MainWindow::applyPcbLive() {
    if (m_loading_controls)
        return;
    for (SceneObject *object : selectedObjects()) {
        if (!object->isArtwork())
            continue;
        object->pcb.exposure = m_pcb_exposure->value();
        object->pcb.drill_mode = m_pcb_drill_mode->currentText();
        object->pcb.invert = m_pcb_invert->isChecked();
        object->pcb.invert_margin = m_pcb_invert_margin->value();
        object->pcb.simplify = m_pcb_simplify->value();
        m_default_pcb = object->pcb;
    }
    invalidateSlice();
    scheduleRebuild();
}

void MainWindow::resetTransform() {
    m_loading_controls = true;
    for (int axis = 0; axis < 3; ++axis) {
        m_move[axis]->setValue(0.0);
        m_rotate[axis]->setValue(0.0);
        m_scale[axis]->setValue(1.0);
        m_mirror[axis]->setChecked(false);
    }
    m_loading_controls = false;
    applyTransformLive();
}

void MainWindow::centreSelection() {
    m_move[0]->setValue(0.0);
    m_move[1]->setValue(0.0);
}

void MainWindow::placeSelectionOnBed() { m_move[2]->setValue(0.0); }

void MainWindow::applySupportPreset(int index) {
    if (m_loading_controls || index == 0)
        return;
    if (index == 1) {
        m_support_enabled->setChecked(false);
        return;
    }
    m_support_enabled->setChecked(true);
    const double factor = index == 2 ? 0.7 : index == 4 ? 1.5 : 1.0;
    m_support_tip->setValue(0.5 * factor);
    m_support_trunk->setValue(1.2 * factor);
    m_support_base->setValue(4.5 * factor);
    m_support_spacing->setValue(index == 2 ? 5.0 : index == 4 ? 3.0 : 4.0);
    applySupportsLive();
}

void MainWindow::addManualSupport() {
    SceneObject *model = firstSelectedModel();
    if (!model)
        return;
    bool accepted = false;
    const QString value = QInputDialog::getText(
        this, "Manual support", "Contact X,Y,Z[,BASE_Z] in millimetres:",
        QLineEdit::Normal, QString(), &accepted);
    if (!accepted)
        return;
    const QStringList fields = value.split(',', Qt::SkipEmptyParts);
    if (fields.size() != 3 && fields.size() != 4) {
        QMessageBox::warning(this, "Manual support", "Enter three or four numbers.");
        return;
    }
    double values[4]{};
    for (int index = 0; index < fields.size(); ++index) {
        bool ok = false;
        values[index] = fields[index].trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(values[index])) {
            QMessageBox::warning(this, "Manual support", "Invalid numeric coordinate.");
            return;
        }
    }
    ManualSupport point;
    point.contact = QVector3D(values[0], values[1], values[2]);
    if (fields.size() == 4)
        point.base_z = values[3];
    model->supports.manual.push_back(point);
    model->supports.enabled = true;
    refreshManualSupports();
    invalidateSlice();
}

void MainWindow::removeManualSupport() {
    SceneObject *model = firstSelectedModel();
    const int row = m_manual_supports->currentRow();
    if (!model || row < 0 || row >= static_cast<int>(model->supports.manual.size()))
        return;
    model->supports.manual.erase(model->supports.manual.begin() + row);
    refreshManualSupports();
    invalidateSlice();
}

void MainWindow::refreshManualSupports() {
    m_manual_supports->clear();
    SceneObject *model = firstSelectedModel();
    if (!model)
        return;
    for (const ManualSupport &point : model->supports.manual) {
        QString label = triplet(point.contact);
        if (point.base_z)
            label += ", base Z=" + number(*point.base_z);
        m_manual_supports->addItem(label);
    }
}

void MainWindow::deleteSelection() {
    const auto old_size = m_objects.size();
    m_objects.erase(std::remove_if(m_objects.begin(), m_objects.end(),
                                   [](const auto &object) { return object->selected; }),
                    m_objects.end());
    if (m_objects.size() == old_size)
        return;
    refreshObjectList();
    refreshView();
    refreshPropertyPanels();
    invalidateSlice();
}

void MainWindow::autoLayout() {
    std::vector<SceneObject *> objects;
    for (auto &object : m_objects)
        if (object->editable && object->visible)
            objects.push_back(object.get());
    if (objects.empty())
        return;
    const double margin = 5.0;
    const double min_x = -m_zone[0]->value() * 0.5 + margin;
    const double max_x = m_zone[0]->value() * 0.5 - margin;
    const double min_y = -m_zone[1]->value() * 0.5 + margin;
    const double max_y = m_zone[1]->value() * 0.5 - margin;
    double cursor_x = min_x;
    double cursor_y = min_y;
    double row_depth = 0.0;
    for (SceneObject *object : objects) {
        const double width = object->visual.maximum.x() - object->visual.minimum.x();
        const double depth = object->visual.maximum.y() - object->visual.minimum.y();
        if (cursor_x + width > max_x) {
            cursor_x = min_x;
            cursor_y += row_depth + margin;
            row_depth = 0.0;
        }
        if (cursor_y + depth > max_y) {
            QMessageBox::warning(this, "Auto layout", "Objects do not fit the printing zone.");
            return;
        }
        const QVector3D current = (object->visual.minimum + object->visual.maximum) * 0.5f;
        const QVector3D target(cursor_x + width * 0.5, cursor_y + depth * 0.5, current.z());
        const QVector3D delta = target - current;
        object->move += delta;
        transformVisual(*object, static_cast<int>(PrintView::GizmoMode::Move), delta);
        cursor_x += width + margin;
        row_depth = std::max(row_depth, depth);
    }
    loadTransformControls();
    refreshView();
    invalidateSlice();
}

void MainWindow::gizmoTransform(int mode, const QVector3D &delta) {
    for (SceneObject *object : selectedObjects()) {
        if (!object->editable)
            continue;
        if (mode == static_cast<int>(PrintView::GizmoMode::Move))
            object->move += delta;
        else if (mode == static_cast<int>(PrintView::GizmoMode::Rotate))
            object->rotate += delta;
        else
            object->scale *= delta;
        transformVisual(*object, mode, delta);
    }
    loadTransformControls();
    refreshView();
    invalidateSlice();
}

void MainWindow::transformVisual(SceneObject &object, int mode, const QVector3D &delta) {
    const QVector3D centre = (object.visual.minimum + object.visual.maximum) * 0.5f;
    const float radians = delta.z() * float(M_PI / 180.0);
    auto transform = [&](QVector3D point) {
        if (mode == static_cast<int>(PrintView::GizmoMode::Move))
            return point + delta;
        point -= centre;
        if (mode == static_cast<int>(PrintView::GizmoMode::Rotate)) {
            const float x = point.x() * std::cos(radians) - point.y() * std::sin(radians);
            const float y = point.x() * std::sin(radians) + point.y() * std::cos(radians);
            point.setX(x);
            point.setY(y);
        } else {
            point *= delta;
        }
        return point + centre;
    };
    auto transform_vertices = [&](QVector<float> &vertices) {
        for (qsizetype index = 0; index + 5 < vertices.size(); index += 6) {
            QVector3D point(vertices[index], vertices[index + 1], vertices[index + 2]);
            point = transform(point);
            vertices[index] = point.x();
            vertices[index + 1] = point.y();
            vertices[index + 2] = point.z();
        }
    };
    transform_vertices(object.visual.model_vertices);
    transform_vertices(object.visual.support_vertices);
    for (LayerData &layer : object.visual.layers) {
        for (QVector<QVector2D> &contour : layer.contours) {
            for (QVector2D &point : contour) {
                const QVector3D transformed = transform(QVector3D(point.x(), point.y(), layer.z));
                point = QVector2D(transformed.x(), transformed.y());
            }
        }
        if (mode == static_cast<int>(PrintView::GizmoMode::Move))
            layer.z += delta.z();
    }
    QVector3D minimum(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max());
    QVector3D maximum = -minimum;
    bool found = false;
    auto include = [&](const QVector3D &point) {
        found = true;
        minimum.setX(std::min(minimum.x(), point.x()));
        minimum.setY(std::min(minimum.y(), point.y()));
        minimum.setZ(std::min(minimum.z(), point.z()));
        maximum.setX(std::max(maximum.x(), point.x()));
        maximum.setY(std::max(maximum.y(), point.y()));
        maximum.setZ(std::max(maximum.z(), point.z()));
    };
    for (const QVector<float> *vertices : {&object.visual.model_vertices,
                                           &object.visual.support_vertices})
        for (qsizetype index = 0; index + 5 < vertices->size(); index += 6)
            include(QVector3D((*vertices)[index], (*vertices)[index + 1],
                              (*vertices)[index + 2]));
    for (const LayerData &layer : object.visual.layers)
        for (const auto &contour : layer.contours)
            for (const QVector2D &point : contour)
                include(QVector3D(point.x(), point.y(), layer.z));
    if (found) {
        object.visual.minimum = minimum;
        object.visual.maximum = maximum;
    }
}

void MainWindow::scheduleRebuild() {
    if (m_rebuild_timer)
        m_rebuild_timer->start();
}

void MainWindow::rebuildSelectedPreviews() {
    for (SceneObject *object : selectedObjects())
        if (object->editable && !rebuildObject(*object, false))
            appendLog("Preview rebuild failed for " + object->display_name);
    refreshView();
}

bool MainWindow::rebuildObject(SceneObject &object, bool show_error) {
    QString project_path;
    QString error;
    if (!writeTemporaryProject(project_path, error, &object,
                               object.type == SceneObjectType::Drill)) {
        if (show_error)
            QMessageBox::critical(this, "Preview", error);
        return false;
    }
    const QString scene_path = makeTemporaryPath(".acv");
    const ProcessResult result = runRust({"project-viewer-scene", project_path, scene_path},
                                         "Preparing " + object.display_name,
                                         60 * 60 * 1000);
    QFile::remove(project_path);
    if (!result.ok) {
        QFile::remove(scene_path);
        if (show_error)
            QMessageBox::critical(this, "Preview", result.output);
        return false;
    }
    SceneData visual;
    const bool loaded = loadScene(scene_path, visual, error);
    QFile::remove(scene_path);
    if (!loaded) {
        if (show_error)
            QMessageBox::critical(this, "Preview", error);
        return false;
    }
    object.visual = std::move(visual);
    return true;
}

void MainWindow::updateLayerControls() {
    const int count = m_view->layerCount();
    m_layer_slider->setRange(0, std::max(0, count - 1));
    m_layer_slider->setEnabled(m_view_mode->currentIndex() == 1 && count > 0);
    updateLayerLabel(m_layer_slider->value());
}

void MainWindow::updateLayerLabel(int layer) {
    if (m_view->layerCount() == 0) {
        m_layer_label->setText("No sliced layers");
        return;
    }
    m_layer_label->setText(QString("Layer %1 / %2, Z=%3 mm")
                               .arg(layer + 1)
                               .arg(m_view->layerCount())
                               .arg(number(m_view->layerZ(layer))));
}

void MainWindow::printingZoneChanged() {
    if (!m_view || !m_zone[0])
        return;
    m_view->setBuildVolume(
        QVector3D(m_zone[0]->value(), m_zone[1]->value(), m_zone[2]->value()));
    invalidateSlice();
}

void MainWindow::invalidateSlice() {
    if (!m_last_pwsz.isEmpty())
        appendLog("Scene changed: prepared PWSZ invalidated.");
    m_last_pwsz.clear();
    m_cloud_file_id.reset();
    m_sliced_layers.clear();
    if (m_view)
        m_view->setLayers({});
    if (m_output_label)
        m_output_label->setText("No prepared PWSZ. Press Slice.");
    if (m_cloud_file_label)
        m_cloud_file_label->setText("Cloud file: not uploaded");
    updateLayerControls();
    updateActions();
}

void MainWindow::updateActions() {
    if (!m_slice_action)
        return;
    const bool sliceable = std::any_of(m_objects.begin(), m_objects.end(), [](const auto &object) {
        return object->editable && object->visible;
    });
    m_layout_action->setEnabled(sliceable);
    m_slice_action->setEnabled(sliceable);
    m_run_action->setEnabled(!m_last_pwsz.isEmpty());
    m_pause_action->setEnabled(!m_printer_id->text().trimmed().isEmpty());
    m_resume_action->setEnabled(m_pause_action->isEnabled());
    m_stop_action->setEnabled(m_pause_action->isEnabled());
    m_status_action->setEnabled(QFileInfo(m_cloud_config->text()).isFile());
}

bool MainWindow::sliceScene() {
    if (m_rebuild_timer->isActive()) {
        m_rebuild_timer->stop();
        rebuildSelectedPreviews();
    }
    if (m_objects.empty())
        return false;
    QString suggested = QDir::current().filePath("plate.pwsz");
    for (const auto &object : m_objects) {
        if (object->editable) {
            suggested = QFileInfo(object->path).absoluteDir().filePath("plate.pwsz");
            break;
        }
    }
    QString output = QFileDialog::getSaveFileName(this, "Write sliced job", suggested,
                                                  "Anycubic print job (*.pwsz)");
    if (output.isEmpty())
        return false;
    if (QFileInfo(output).suffix().isEmpty())
        output += ".pwsz";
    QString project_path;
    QString error;
    if (!writeTemporaryProject(project_path, error)) {
        QMessageBox::critical(this, "Slice", error);
        return false;
    }
    const ProcessResult sliced = runRust({"project-slice", project_path, output},
                                         "Slicing unified scene", 60 * 60 * 1000);
    if (!sliced.ok) {
        QFile::remove(project_path);
        QMessageBox::critical(this, "Slice", sliced.output);
        return false;
    }
    const QString scene_path = makeTemporaryPath(".acv");
    const ProcessResult preview = runRust(
        {"project-viewer-scene", project_path, scene_path}, "Preparing sliced layers",
        60 * 60 * 1000);
    QFile::remove(project_path);
    if (!preview.ok) {
        QFile::remove(scene_path);
        QMessageBox::warning(this, "Slice preview", preview.output);
        return false;
    }
    SceneData scene;
    if (!loadScene(scene_path, scene, error)) {
        QFile::remove(scene_path);
        QMessageBox::warning(this, "Slice preview", error);
        return false;
    }
    QFile::remove(scene_path);
    m_last_pwsz = QFileInfo(output).absoluteFilePath();
    m_cloud_file_id.reset();
    m_sliced_layers = scene.layers;
    const bool has_model = std::any_of(m_objects.begin(), m_objects.end(), [](const auto &object) {
        return object->visible && object->isModel();
    });
    m_last_job_is_pcb = !has_model;
    m_last_pcb_exposure = 0.0;
    for (const auto &object : m_objects)
        if (object->visible && object->isArtwork()) {
            m_last_pcb_exposure = object->pcb.exposure;
            break;
        }
    for (auto &object : m_objects)
        if (object->isModel())
            object->visual.support_vertices.clear();
    for (auto &object : m_objects)
        if (object->visible && object->isModel()) {
            object->visual.support_vertices = scene.support_vertices;
            break;
        }
    m_view->setLayers(m_sliced_layers);
    m_view_mode->setCurrentIndex(1);
    m_layer_slider->setValue(0);
    m_output_label->setText("Prepared PWSZ: " + m_last_pwsz);
    m_cloud_file_label->setText("Cloud file: not uploaded");
    refreshView();
    updateActions();
    appendLog("Slice complete: " + m_last_pwsz);
    return true;
}

void MainWindow::runPrepared() {
    if (m_last_pwsz.isEmpty()) {
        QMessageBox::warning(this, "Run", "Press Slice first. Run never re-slices implicitly.");
        return;
    }
    if (!uploadToCloud())
        return;
    startPrinting();
}

bool MainWindow::uploadToCloud() {
    qulonglong printer_id = 0;
    if (!validateCloudInputs(printer_id))
        return false;
    const ProcessResult result = runRust(
        {"cloud-upload", m_cloud_config->text(), QString::number(printer_id), m_last_pwsz},
        "Uploading prepared PWSZ", 60 * 60 * 1000);
    if (!result.ok) {
        QMessageBox::critical(this, "Cloud upload", result.output);
        return false;
    }
    const QRegularExpression expression("Cloud file\\s+(\\d+)");
    const auto match = expression.match(result.output);
    if (!match.hasMatch()) {
        QMessageBox::critical(this, "Cloud upload",
                              "Upload response contained no cloud file ID. Start was blocked.\n" +
                                  result.output);
        return false;
    }
    m_cloud_file_id = match.captured(1).toULongLong();
    m_cloud_file_label->setText("Cloud file ID: " + QString::number(*m_cloud_file_id));
    return true;
}

void MainWindow::startPrinting() {
    if (!m_cloud_file_id)
        return;
    qulonglong printer_id = 0;
    if (!validateCloudInputs(printer_id))
        return;
    QString warning;
    if (m_last_job_is_pcb) {
        warning = QString("Start a REAL PCB exposure for %1 s on printer %2? The sliced LCD "
                          "pattern is ready, but firmware may still home/move Z. Remove the "
                          "platform and use UV eye/skin protection.")
                      .arg(number(m_last_pcb_exposure), QString::number(printer_id));
    } else {
        warning = "Start a real resin print on printer " + QString::number(printer_id) +
                  "? Verify vat, resin, platform and the sliced layers.";
    }
    if (QMessageBox::warning(this, "Start real printer", warning,
                             QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    const QString operation = m_last_job_is_pcb ? "cloud-start-pcb" : "cloud-start";
    const ProcessResult result = runRust(
        {operation, m_cloud_config->text(), QString::number(printer_id),
         QString::number(*m_cloud_file_id)},
        "Starting prepared job");
    if (result.ok)
        QMessageBox::information(this, "Run", result.output);
    else
        QMessageBox::critical(this, "Run", result.output);
}

void MainWindow::browseCloudConfig() {
    const QString path = QFileDialog::getOpenFileName(
        this, "Workshop account config", m_cloud_config->text(),
        "Configuration (*.ini);;All files (*)");
    if (!path.isEmpty()) {
        m_cloud_config->setText(QFileInfo(path).absoluteFilePath());
        saveSettings();
        updateActions();
    }
}

void MainWindow::loginToCloud() {
    QString config = m_cloud_config->text().trimmed();
    if (config.isEmpty()) {
        config = QFileDialog::getSaveFileName(this, "Store Anycubic token",
                                              QDir::home().filePath("anycubic-cloud.ini"),
                                              "Configuration (*.ini)");
        if (config.isEmpty())
            return;
        if (QFileInfo(config).suffix().isEmpty())
            config += ".ini";
    }
    const QUrl url(
        "https://uc.makeronline.com/login/oauth/authorize?client_id=25ec844d72180b556acc&"
        "response_type=code&scope=read&application=ac_slicer_lcd&redirect_uri=acpws%3A%2F%2Flogin");
    QDesktopServices::openUrl(url);
    bool accepted = false;
    const QString callback = QInputDialog::getText(
        this, "Complete Anycubic login",
        "After browser login, paste the acpws://login?... callback (or code):",
        QLineEdit::Normal, QString(), &accepted);
    if (!accepted || callback.trimmed().isEmpty())
        return;
    const ProcessResult result = runRust(
        {"cloud-login-complete", QFileInfo(config).absoluteFilePath()},
        "Completing OAuth login", 60 * 1000, callback.trimmed().toUtf8());
    if (!result.ok) {
        QMessageBox::critical(this, "Login", result.output);
        return;
    }
    m_cloud_config->setText(QFileInfo(config).absoluteFilePath());
    connectToCloud(true);
}

bool MainWindow::connectToCloud(bool show_result) {
    if (!QFileInfo(m_cloud_config->text()).isFile())
        return false;
    const ProcessResult status = runRust({"cloud-status", m_cloud_config->text()},
                                         "Reading Cloud printers", 60 * 1000);
    if (!status.ok) {
        if (show_result)
            QMessageBox::critical(this, "Cloud", status.output);
        return false;
    }
    QRegularExpression expression("id=Some\\((\\d+)\\)");
    auto match = expression.match(status.output);
    if (!match.hasMatch()) {
        expression.setPattern("id=(\\d+)");
        match = expression.match(status.output);
    }
    if (match.hasMatch() && m_printer_id->text().trimmed().isEmpty())
        m_printer_id->setText(match.captured(1));
    saveSettings();
    updateActions();
    if (show_result)
        QMessageBox::information(this, "Cloud", status.output);
    return true;
}

bool MainWindow::validateCloudInputs(qulonglong &printer_id) {
    if (!QFileInfo(m_cloud_config->text()).isFile()) {
        QMessageBox::warning(this, "Cloud", "Select a valid account config first.");
        return false;
    }
    bool ok = false;
    printer_id = m_printer_id->text().trimmed().toULongLong(&ok);
    if (!ok || printer_id == 0) {
        QMessageBox::warning(this, "Cloud", "Enter or detect a numeric printer ID.");
        return false;
    }
    return true;
}

void MainWindow::pausePrinting() {
    qulonglong id = 0;
    if (!validateCloudInputs(id))
        return;
    const auto result = runRust(
        {"cloud-pause", m_cloud_config->text(), QString::number(id)}, "Pausing print");
    if (result.ok)
        QMessageBox::information(this, "Pause", result.output);
    else
        QMessageBox::critical(this, "Pause", result.output);
}

void MainWindow::resumePrinting() {
    qulonglong id = 0;
    if (!validateCloudInputs(id))
        return;
    const auto result = runRust(
        {"cloud-resume", m_cloud_config->text(), QString::number(id)}, "Resuming print");
    if (result.ok)
        QMessageBox::information(this, "Resume", result.output);
    else
        QMessageBox::critical(this, "Resume", result.output);
}

void MainWindow::stopPrinting() {
    qulonglong id = 0;
    if (!validateCloudInputs(id))
        return;
    if (QMessageBox::warning(this, "Stop real print",
                             "Resolve the active project automatically and stop it?",
                             QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    const auto result = runRust(
        {"cloud-stop", m_cloud_config->text(), QString::number(id)}, "Stopping print");
    if (result.ok)
        QMessageBox::information(this, "Stop", result.output);
    else
        QMessageBox::critical(this, "Stop", result.output);
}

void MainWindow::requestPrinterStatus() {
    const auto result = runRust({"cloud-status-detail", m_cloud_config->text()},
                                "Reading printer status", 60 * 1000);
    if (result.ok)
        QMessageBox::information(this, "Printer status", result.output);
    else
        QMessageBox::critical(this, "Printer status", result.output);
}

void MainWindow::startBuiltinExposure() {
    qulonglong id = 0;
    if (!validateCloudInputs(id))
        return;
    if (QMessageBox::warning(this, "LCD UV test", "Turn on the built-in UV test pattern?",
                             QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    const auto result = runRust(
        {"cloud-exposure-start", m_cloud_config->text(), QString::number(id),
         QString::number(m_exposure_pattern->currentData().toInt()),
         QString::number(m_exposure_duration->value())},
        "Starting LCD test");
    if (!result.ok)
        QMessageBox::critical(this, "LCD test", result.output);
}

void MainWindow::stopBuiltinExposure() {
    qulonglong id = 0;
    if (!validateCloudInputs(id))
        return;
    const auto result = runRust(
        {"cloud-exposure-stop", m_cloud_config->text(), QString::number(id)},
        "Stopping LCD test");
    if (!result.ok)
        QMessageBox::critical(this, "LCD test", result.output);
}

ProcessResult MainWindow::runRust(const QStringList &arguments, const QString &title,
                                  int timeout_ms, const QByteArray &standard_input) {
    const QString executable = rustCli();
    if (executable.isEmpty())
        return {false, "Could not locate the anycubicstudio Rust CLI."};
    QStringList safe = arguments;
    if (!safe.isEmpty() && safe.front() == "cloud-login-complete")
        safe.append("<code via stdin>");
    appendLog("$ " + executable + " " + safe.join(' '));
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(executable, arguments);
    if (!process.waitForStarted(5000))
        return {false, process.errorString()};
    if (!standard_input.isEmpty()) {
        process.write(standard_input);
        process.closeWriteChannel();
    }
    QProgressDialog progress(title, "Cancel", 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(350);
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(80);
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        if (progress.wasCanceled() || timer.elapsed() > timeout_ms) {
            process.kill();
            process.waitForFinished(3000);
            return {false, progress.wasCanceled() ? "Operation cancelled."
                                                  : "Rust API timed out."};
        }
    }
    const QString output = QString::fromUtf8(process.readAll()).trimmed();
    if (!output.isEmpty())
        appendLog(output);
    const bool ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    return {ok, output.isEmpty() ? (ok ? "Done." : "Rust API failed without output.")
                                 : output};
}

QString MainWindow::rustCli() const {
    const QString environment = qEnvironmentVariable("ANYCUBICSTUDIO_CLI");
    if (!environment.isEmpty() && QFileInfo::exists(environment))
        return QFileInfo(environment).absoluteFilePath();
    for (const QString &candidate : {
             QDir(QCoreApplication::applicationDirPath()).filePath("anycubicstudio"),
             QDir::current().filePath("target/release/anycubicstudio"),
             QDir::current().filePath("target/debug/anycubicstudio")})
        if (QFileInfo::exists(candidate))
            return QFileInfo(candidate).absoluteFilePath();
    return {};
}

QByteArray MainWindow::projectJson(const SceneObject *isolated,
                                   bool expose_isolated_drill) const {
    const QJsonObject default_transform{{"translation_mm", QJsonArray{0.0, 0.0, 0.0}},
                                        {"rotation_deg", QJsonArray{0.0, 0.0, 0.0}},
                                        {"scale", QJsonArray{1.0, 1.0, 1.0}},
                                        {"auto_center_xy", true},
                                        {"place_on_bed", true}};
    SupportSettings disabled;
    const QJsonObject machine{
        {"name", "Anycubic Photon Mono M7 Pro"},
        {"resolution", QJsonArray{13312, 5120}},
        {"build_volume_mm",
         QJsonArray{m_zone[0]->value(), m_zone[1]->value(), m_zone[2]->value()}},
        {"pixel_size_um", QJsonArray{16.799999237060547, 24.799999237060547}},
        {"max_segments_per_layer", 100000}};
    const QJsonObject print{{"layer_height_mm", m_layer_height->value()},
                            {"bottom_layers", m_bottom_layers->value()},
                            {"bottom_exposure_s", m_bottom_exposure->value()},
                            {"exposure_s", m_exposure->value()},
                            {"lift_height_mm", m_lift_height->value()},
                            {"lift_speed_mm_s", m_lift_speed->value()},
                            {"off_time_s", m_off_time->value()},
                            {"resin_density_g_ml", 1.2},
                            {"resin_price", 220.0},
                            {"resin_volume_ml", 1000.0},
                            {"currency", "$"}};
    const QJsonObject config{{"machine", machine},
                             {"print", print},
                             {"transform", default_transform},
                             {"supports", supportJson(disabled)},
                             {"xy_offset_mm", 0.0},
                             {"hollow", false},
                             {"gmsh_path", "gmsh"}};
    QJsonArray objects;
    const auto append_object = [&](const SceneObject *object) {
        if (!object || !object->editable || (!isolated && !object->visible))
            return;
        QString drill_mode = object->pcb.drill_mode;
        if (isolated == object && expose_isolated_drill)
            drill_mode = "expose";
        const QJsonObject pcb{{"exposure_s", object->pcb.exposure},
                              {"auto_mirror_front_layers", false},
                              {"invert", object->pcb.invert},
                              {"invert_margin_mm", object->pcb.invert_margin},
                              {"drill_mode", drill_mode},
                              {"simplify_tolerance_mm", object->pcb.simplify}};
        objects.append(QJsonObject{{"input", object->path},
                                   {"source_type", sourceTypeJson(object->type)},
                                   {"visible", true},
                                   {"transform", transformJson(*object)},
                                   {"supports", supportJson(object->supports)},
                                   {"pcb", pcb}});
    };
    if (isolated) {
        // A newly imported object is previewed before ownership is transferred
        // to m_objects, so it must be serialized directly rather than found in
        // the current scene collection.
        append_object(isolated);
    } else {
        for (const auto &owned : m_objects)
            append_object(owned.get());
    }
    return QJsonDocument(QJsonObject{{"config", config}, {"objects", objects}})
        .toJson(QJsonDocument::Indented);
}

QString MainWindow::makeTemporaryPath(const QString &suffix) const {
    QTemporaryFile file(QDir::temp().filePath("anycubic-gui-XXXXXX" + suffix));
    file.setAutoRemove(false);
    if (!file.open())
        return {};
    const QString path = file.fileName();
    file.close();
    QFile::remove(path);
    return path;
}

bool MainWindow::writeTemporaryProject(QString &path, QString &error,
                                       const SceneObject *isolated,
                                       bool expose_isolated_drill) const {
    path = makeTemporaryPath(".json");
    if (path.isEmpty()) {
        error = "Could not allocate a temporary project file.";
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = file.errorString();
        return false;
    }
    if (file.write(projectJson(isolated, expose_isolated_drill)) < 0) {
        error = file.errorString();
        file.remove();
        return false;
    }
    return true;
}

void MainWindow::appendLog(const QString &text) {
    const QString message = text.trimmed();
    if (message.isEmpty())
        return;
    if (m_log)
        m_log->appendPlainText(message);
    if (m_non_interactive)
        qInfo().noquote() << message;
}
