#include "main_window.hpp"

#include "print_view.hpp"
#include "support_preview.hpp"

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
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QScopeGuard>
#include <QScreen>
#include <QSignalBlocker>
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

QJsonObject transformJson(const SceneObject &object, bool automatic_front_group) {
    QVector3D scale = object.scale;
    const bool automatic_front = object.isArtwork() && object.pcb.auto_mirror &&
                                 automatic_front_group;
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
            {"density_per_cm2", support.density},
            {"tip_diameter_mm", support.tip},
            {"trunk_diameter_mm", support.trunk},
            {"base_diameter_mm", support.base},
            {"tip_height_mm", support.tip_height},
            {"base_height_mm", support.base_height},
            {"penetration_mm", support.penetration},
            {"min_height_mm", support.minimum_height},
            {"max_height_mm", support.maximum_height},
            {"branch_max_distance_mm", support.branch_max_distance},
            {"branch_max_children", support.branch_max_children},
            {"branch_max_trunk_diameter_mm", support.branch_max_trunk},
            {"radial_segments", support.segments},
            {"manual_points", manual}};
}

int supportPresetIndex(const SupportSettings &support) {
    if (!support.enabled)
        return 1; // None
    if (!support.automatic || support.shape != "branch" || !support.manual.empty())
        return 0;
    const auto close = [](double left, double right) {
        return std::abs(left - right) <= 1.0e-6;
    };
    const bool common = close(support.angle, 45.0) && close(support.base_height, 0.8) &&
                        close(support.penetration, 0.15) &&
                        close(support.minimum_height, 2.0) &&
                        close(support.maximum_height, 180.0) &&
                        close(support.branch_max_distance, 30.0) &&
                        support.branch_max_children == 6 && support.segments == 12;
    if (!common)
        return 0;
    if (close(support.spacing, 5.0) && close(support.tip, 0.35) &&
        close(support.trunk, 0.8) && close(support.base, 3.5) &&
        close(support.tip_height, 1.5) && close(support.density, 4.0) &&
        close(support.branch_max_trunk, 2.5))
        return 2; // Light
    if (close(support.spacing, 4.0) && close(support.tip, 0.5) &&
        close(support.trunk, 1.2) && close(support.base, 4.5) &&
        close(support.tip_height, 1.5) && close(support.density, 6.25) &&
        close(support.branch_max_trunk, 3.2))
        return 3; // Medium
    if (close(support.spacing, 3.0) && close(support.tip, 0.8) &&
        close(support.trunk, 1.8) && close(support.base, 6.0) &&
        close(support.tip_height, 2.0) && close(support.density, 100.0 / 9.0) &&
        close(support.branch_max_trunk, 4.5))
        return 4; // Heavy
    return 0;
}

SupportSettings supportFromJson(const QJsonObject &object) {
    SupportSettings support;
    support.enabled = object.value("enabled").toBool(support.enabled);
    support.automatic = object.value("auto_generate").toBool(support.automatic);
    support.shape = object.value("shape").toString(support.shape);
    support.angle = object.value("overhang_angle_deg").toDouble(support.angle);
    support.spacing = object.value("spacing_mm").toDouble(support.spacing);
    support.density = object.value("density_per_cm2").toDouble(support.density);
    support.tip = object.value("tip_diameter_mm").toDouble(support.tip);
    support.trunk = object.value("trunk_diameter_mm").toDouble(support.trunk);
    support.base = object.value("base_diameter_mm").toDouble(support.base);
    support.tip_height = object.value("tip_height_mm").toDouble(support.tip_height);
    support.base_height = object.value("base_height_mm").toDouble(support.base_height);
    support.penetration = object.value("penetration_mm").toDouble(support.penetration);
    support.minimum_height = object.value("min_height_mm").toDouble(support.minimum_height);
    support.maximum_height = object.value("max_height_mm").toDouble(support.maximum_height);
    support.branch_max_distance =
        object.value("branch_max_distance_mm").toDouble(support.branch_max_distance);
    support.branch_max_children =
        object.value("branch_max_children").toInt(support.branch_max_children);
    support.branch_max_trunk = object.value("branch_max_trunk_diameter_mm")
                                   .toDouble(support.branch_max_trunk);
    support.segments = object.value("radial_segments").toInt(support.segments);
    for (const QJsonValue &entry : object.value("manual_points").toArray()) {
        const QJsonObject point_object = entry.toObject();
        const QJsonArray coordinates = point_object.value("contact_mm").toArray();
        if (coordinates.size() != 3)
            continue;
        ManualSupport point;
        point.contact = QVector3D(coordinates[0].toDouble(), coordinates[1].toDouble(),
                                  coordinates[2].toDouble());
        const QJsonValue base_z = point_object.value("base_z_mm");
        if (base_z.isDouble())
            point.base_z = base_z.toDouble();
        support.manual.push_back(point);
    }
    return support;
}

bool sameSupportSettings(const SupportSettings &left, const SupportSettings &right) {
    return QJsonDocument(supportJson(left)).toJson(QJsonDocument::Compact) ==
           QJsonDocument(supportJson(right)).toJson(QJsonDocument::Compact);
}

} // namespace

MainWindow::MainWindow(const QStringList &initial_files, QWidget *parent, bool non_interactive)
    : QMainWindow(parent), m_non_interactive(non_interactive) {
    setWindowTitle("Anycubic Studio — unified plate editor");
    resize(1500, 900);
    setAcceptDrops(true);
    buildInterface();
    qApp->installEventFilter(this);
    connectActions();
    loadSettings();
    printingZoneChanged();
    if (!initial_files.isEmpty())
        importPaths(initial_files);
    updateActions();
    syncPrintSettingsHistoryBaseline();
    m_history_enabled = true;
    updateHistoryActions();
    if (!m_non_interactive && QFileInfo(m_cloud_config->text()).isFile())
        QTimer::singleShot(0, this, [this] { connectToCloud(false); });
}

void MainWindow::buildInterface() {
    auto *edit_menu = menuBar()->addMenu("&Edit");
    m_undo_action = edit_menu->addAction(QIcon(":/icons/undo.png"), "Undo");
    m_undo_action->setShortcut(QKeySequence::Undo);
    m_redo_action = edit_menu->addAction(QIcon(":/icons/redo.png"), "Redo");
    m_redo_action->setShortcut(QKeySequence("Ctrl+Shift+Z"));
    connect(m_undo_action, &QAction::triggered, this, [this] { undo(); });
    connect(m_redo_action, &QAction::triggered, this, [this] { redo(); });

    auto *toolbar = addToolBar("Workflow");
    toolbar->setObjectName("workflowToolbar");
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setIconSize(QSize(22, 22));
    toolbar->setStyleSheet("QToolButton { padding: 1px; margin: 0px; }");
    toolbar->setMovable(false);
    // The supplied artwork is deliberately the only icon source. Do not mix it
    // with platform-dependent QStyle icons: they vary between desktops and can
    // misleadingly depict a different operation.
    m_import_action = toolbar->addAction(QIcon(":/icons/import.png"), "Import…");
    toolbar->addAction(m_undo_action);
    toolbar->addAction(m_redo_action);
    m_layout_action = toolbar->addAction(QIcon(":/icons/layout.png"), "Auto layout");
    toolbar->addSeparator();
    m_slice_action = toolbar->addAction(QIcon(":/icons/slice.png"), "Slice…");
    m_run_action = toolbar->addAction(QIcon(":/icons/run.png"), "Run…");
    toolbar->addSeparator();
    auto *gizmo_group = new QActionGroup(this);
    gizmo_group->setExclusive(true);
    QAction *move = toolbar->addAction(QIcon(":/icons/move.png"), "Move");
    QAction *rotate = toolbar->addAction(QIcon(":/icons/rotate.png"), "Rotate");
    QAction *scale = toolbar->addAction(QIcon(":/icons/scale.png"), "Scale");
    for (QAction *action : {move, rotate, scale}) {
        action->setCheckable(true);
        gizmo_group->addAction(action);
    }
    move->setChecked(true);
    m_fit_action = toolbar->addAction(QIcon(":/icons/fit.png"), "Fit view");
    toolbar->addSeparator();
    m_pause_action = toolbar->addAction(QIcon(":/icons/pause.png"), "Pause");
    m_resume_action = toolbar->addAction(QIcon(":/icons/resume.png"), "Resume");
    m_stop_action = toolbar->addAction(QIcon(":/icons/abort.png"), "Stop");
    m_status_action = toolbar->addAction(QIcon(":/icons/status.png"), "Printer status");

    connect(m_import_action, &QAction::triggered, this, [this] { importDialog(); });
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

    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks |
                   QMainWindow::AllowTabbedDocks);
    setCentralWidget(buildCentre());
    connect(m_fit_action, &QAction::triggered, m_view, &PrintView::resetCamera);

    auto *object_panel = new QWidget;
    auto *object_layout = new QVBoxLayout(object_panel);
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
    auto *models_dock = new QDockWidget("Models on plate", this);
    models_dock->setObjectName("modelsOnPlateDock");
    models_dock->setWidget(object_panel);
    models_dock->setMinimumWidth(365);

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
    auto *settings_dock = new QDockWidget("Settings", this);
    settings_dock->setObjectName("settingsDock");
    settings_dock->setWidget(m_tabs);
    settings_dock->setMinimumWidth(365);
    addDockWidget(Qt::RightDockWidgetArea, settings_dock);

    addDockWidget(Qt::RightDockWidgetArea, models_dock);
    splitDockWidget(settings_dock, models_dock, Qt::Vertical);

    m_layer_dock = new QDockWidget("Layers", this);
    m_layer_dock->setObjectName("layersDock");
    m_layer_dock->setWidget(buildLayersPanel());
    m_layer_dock->setMinimumWidth(365);
    addDockWidget(Qt::RightDockWidgetArea, m_layer_dock);
    splitDockWidget(models_dock, m_layer_dock, Qt::Vertical);

    auto *dock = new QDockWidget("Rust API output", this);
    dock->setObjectName("rustApiOutputDock");
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    dock->setWidget(m_log);
    dock->setMinimumHeight(120);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    dock->hide();

    auto *view_menu = menuBar()->addMenu("&View");
    view_menu->addAction(models_dock->toggleViewAction());
    view_menu->addAction(settings_dock->toggleViewAction());
    view_menu->addAction(m_layer_dock->toggleViewAction());
    view_menu->addAction(dock->toggleViewAction());

    m_history_timer = new QTimer(this);
    m_history_timer->setSingleShot(true);
    m_history_timer->setInterval(400);
    connect(m_history_timer, &QTimer::timeout, this, [this] {
        m_history_coalescing = false;
    });
    appendLog("Unified scene ready. Slice prepares a PWSZ; Run only uploads and starts it.");
}

QWidget *MainWindow::buildCentre() {
    auto *container = new QWidget;
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    m_view = new PrintView;
    layout->addWidget(m_view, 1);
    auto *help = new QLabel(
        "3D: click object to select; drag selected object with the active Move/Rotate/Scale "
        "gizmo. Drag empty space for CAD box selection (right = crossing, left = enclosed); "
        "Middle-button drag orbits; right/Shift-left drag pans. Slice overlays the selected layer in 3D; "
        "Esc or Ctrl+Z exits that overlay.");
    help->setWordWrap(true);
    help->setStyleSheet("color: #8e9bad;");
    layout->addWidget(help);
    return container;
}

QWidget *MainWindow::buildLayersPanel() {
    m_layer_controls = new QWidget;
    auto *layout = new QVBoxLayout(m_layer_controls);
    auto *position = new QHBoxLayout;
    position->addWidget(new QLabel("Layer"));
    m_layer_number = new QSpinBox;
    m_layer_number->setRange(1, 1);
    m_layer_number->setKeyboardTracking(false);
    m_layer_number->setEnabled(false);
    m_layer_number->setMinimumWidth(82);
    position->addWidget(m_layer_number);
    m_layer_label = new QLabel("/ 0, Z=0.000 mm");
    // Fixed label geometry prevents cursor-relative slider feedback.
    m_layer_label->setFixedWidth(175);
    position->addWidget(m_layer_label);
    position->addStretch();
    layout->addLayout(position);
    m_layer_slider = new QSlider(Qt::Horizontal);
    m_layer_slider->setRange(0, 0);
    m_layer_slider->setEnabled(false);
    layout->addWidget(m_layer_slider);
    const auto add_opacity = [this, layout](const QString &name, QSlider *&slider,
                                            auto setter) {
        auto *name_label = new QLabel(name);
        auto *value_label = new QLabel;
        value_label->setMinimumWidth(38);
        value_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, 100);
        slider->setValue(22);
        slider->setEnabled(false);
        auto *row = new QHBoxLayout;
        row->addWidget(name_label);
        row->addWidget(slider, 1);
        row->addWidget(value_label);
        layout->addLayout(row);
        connect(slider, &QSlider::valueChanged, this,
                [setter, value_label](int value) {
                    value_label->setText(QString::number(value) + "%");
                    setter(value);
                });
        value_label->setText(QString::number(slider->value()) + "%");
    };
    add_opacity("Supports opacity", m_support_opacity,
                [this](int value) { m_view->setSupportOpacity(value); });
    add_opacity("Model opacity", m_model_opacity,
                [this](int value) { m_view->setModelOpacity(value); });
    layout->addStretch();

    m_layer_timer = new QTimer(this);
    m_layer_timer->setSingleShot(true);
    m_layer_timer->setInterval(20);
    m_layer_timer->setTimerType(Qt::PreciseTimer);
    connect(m_layer_slider, &QSlider::valueChanged, this, [this](int value) {
        const QSignalBlocker blocker(m_layer_number);
        m_layer_number->setValue(value + 1);
        updateLayerLabel(value);
        if (!m_layer_timer->isActive())
            m_layer_timer->start();
    });
    connect(m_layer_number, &QSpinBox::valueChanged, this,
            [this](int value) { m_layer_slider->setValue(value - 1); });
    connect(m_layer_slider, &QSlider::sliderReleased, this, [this] {
        m_layer_timer->stop();
        m_view->setLayer(m_layer_slider->value());
    });
    connect(m_layer_timer, &QTimer::timeout, this,
            [this] { m_view->setLayer(m_layer_slider->value()); });
    return m_layer_controls;
}

QWidget *MainWindow::buildTransformPanel() {
    auto *panel = new QWidget;
    auto *layout = new QVBoxLayout(panel);
    auto *form = new QFormLayout;
    for (int axis = 0; axis < 3; ++axis) {
        m_move[axis] = spin(-1000.0, 1000.0, 0.0, 3, 1.0);
        m_rotate[axis] = spin(-360.0, 360.0, 0.0, 1, 15.0);
        m_scale[axis] = spin(1.0, 1000.0, 1.0, 0, 1.0);
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
    auto *snap_pcb = new QPushButton("Snap PCB layers");
    layout->addWidget(rowOf({centre, bed}));
    layout->addWidget(rowOf({reset, snap_pcb}));
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
    connect(snap_pcb, &QPushButton::clicked, this, [this] { snapArtworkSelection(); });
    return panel;
}

QWidget *MainWindow::buildSupportPanel() {
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    auto *form = new QFormLayout;
    m_support_form = form;
    m_support_preset = new QComboBox;
    m_support_preset->addItems(
        {"Custom", "None", "Light", "Medium", "Heavy", "+ Add new preset"});
    m_support_preset->setContextMenuPolicy(Qt::CustomContextMenu);
    m_support_enabled = new QCheckBox("Generate supports");
    m_support_auto = new QCheckBox("Automatic placement");
    m_support_shape = new QComboBox;
    m_support_shape->addItems({"branch", "tapered", "cylinder", "square"});
    m_support_angle = spin(1.1, 88.9, 45.0, 1, 1.0);
    m_support_spacing = spin(0.1, 100.0, 4.0, 3, 0.1);
    m_support_density = spin(0.1, 1000.0, 6.25, 2, 0.1);
    m_support_tip = spin(0.05, 20.0, 0.5, 3, 0.1);
    m_support_trunk = spin(0.05, 30.0, 1.2, 3, 0.1);
    m_support_base = spin(0.05, 50.0, 4.5, 3, 0.1);
    m_support_tip_height = spin(0.01, 50.0, 1.5, 3, 0.1);
    m_support_base_height = spin(0.01, 50.0, 0.8, 3, 0.1);
    m_support_penetration = spin(0.0, 10.0, 0.15, 3, 0.1);
    m_support_min_height = spin(0.0, 500.0, 2.0, 3, 0.1);
    m_support_max_height = spin(0.01, 1000.0, 180.0, 3, 0.1);
    m_support_branch_max_distance = spin(0.1, 500.0, 30.0, 2, 0.1);
    m_support_branch_max_children = new QSpinBox;
    m_support_branch_max_children->setRange(1, 32);
    m_support_branch_max_children->setValue(6);
    m_support_branch_max_trunk = spin(0.05, 50.0, 3.2, 3, 0.1);
    m_support_segments = new QSpinBox;
    m_support_segments->setRange(6, 128);
    m_support_segments->setValue(12);
    m_support_preview = new SupportPreview(this);
    m_support_preview_controls = {
        m_support_preset,      m_support_enabled,     m_support_auto,
        m_support_shape,       m_support_angle,       m_support_spacing,
        m_support_density,     m_support_tip,         m_support_trunk,
        m_support_base,        m_support_tip_height,  m_support_base_height,
        m_support_penetration, m_support_min_height,  m_support_max_height,
        m_support_branch_max_distance, m_support_branch_max_children,
        m_support_branch_max_trunk, m_support_segments,
    };
    form->addRow("Preset", m_support_preset);
    form->addRow("Enabled", m_support_enabled);
    form->addRow("Placement", m_support_auto);
    form->addRow("Shape", m_support_shape);
    form->addRow("Overhang angle (°)", m_support_angle);
    form->addRow("Spacing (mm)", m_support_spacing);
    form->addRow("Density (contacts/cm²)", m_support_density);
    form->addRow("Tip diameter (mm)", m_support_tip);
    form->addRow("Trunk diameter (mm)", m_support_trunk);
    form->addRow("Base diameter (mm)", m_support_base);
    form->addRow("Tip height (mm)", m_support_tip_height);
    form->addRow("Base height (mm)", m_support_base_height);
    form->addRow("Penetration (mm)", m_support_penetration);
    form->addRow("Min height (mm)", m_support_min_height);
    form->addRow("Max height (mm)", m_support_max_height);
    form->addRow("Tree reach zone (mm)", m_support_branch_max_distance);
    form->addRow("Max branches per node", m_support_branch_max_children);
    form->addRow("Max shared trunk Ø (mm)", m_support_branch_max_trunk);
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
    connect(m_support_preset, &QComboBox::customContextMenuRequested, this,
            [this](const QPoint &position) {
                QMenu menu(this);
                QAction *remove = menu.addAction("Delete current preset");
                const int custom_index = m_support_preset->currentIndex() - 5;
                remove->setEnabled(custom_index >= 0 &&
                                   custom_index <
                                       static_cast<int>(m_custom_support_presets.size()));
                if (menu.exec(m_support_preset->mapToGlobal(position)) == remove &&
                    remove->isEnabled())
                    deleteCurrentSupportPreset();
            });
    connect(add, &QPushButton::clicked, this, [this] { addManualSupport(); });
    connect(remove, &QPushButton::clicked, this, [this] { removeManualSupport(); });
    for (QCheckBox *box : {m_support_enabled, m_support_auto})
        connect(box, &QCheckBox::toggled, this, [this] { applySupportsLive(); });
    connect(m_support_shape, &QComboBox::currentIndexChanged, this, [this] {
        updateSupportControlVisibility();
        applySupportsLive();
    });
    for (QDoubleSpinBox *value : {m_support_angle,
                                  m_support_spacing,
                                  m_support_density,
                                  m_support_tip,
                                  m_support_trunk,
                                  m_support_base,
                                  m_support_tip_height,
                                  m_support_base_height,
                                  m_support_penetration,
                                  m_support_min_height,
                                  m_support_max_height,
                                  m_support_branch_max_distance,
                                  m_support_branch_max_trunk})
        connect(value, &QDoubleSpinBox::valueChanged, this,
                [this, value] {
                    enforceSupportConstraints(value);
                    applySupportsLive();
                });
    connect(m_support_branch_max_children, &QSpinBox::valueChanged, this,
            [this] { applySupportsLive(); });
    connect(m_support_segments, &QSpinBox::valueChanged, this,
            [this] { applySupportsLive(); });
    enforceSupportConstraints();
    updateSupportControlVisibility();
    updateSupportPreview();
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
    m_pcb_drill_mode->setEnabled(false);
    m_pcb_drill_mode->setToolTip(
        "Deprecated: drill composition is kept for project compatibility; use polarity/invert.");
    m_pcb_invert = new QCheckBox("Invert inside artwork bounds");
    m_pcb_invert_margin = spin(0.0, 1000.0, 0.0);
    m_pcb_simplify = spin(0.0, 10.0, 0.012, 4, 0.002);
    form->addRow("Exposure (s)", m_pcb_exposure);
    form->addRow("Drill pixels (deprecated)", m_pcb_drill_mode);
    form->addRow("Polarity", m_pcb_invert);
    form->addRow("Invert margin (mm)", m_pcb_invert_margin);
    form->addRow("Contour simplify (mm)", m_pcb_simplify);
    layout->addLayout(form);
    m_pcb_status = new QLabel;
    m_pcb_status->setWordWrap(true);
    layout->addWidget(m_pcb_status);
    auto *note = new QLabel(
        "Move/Rotate/Mirror/Scale are in the common Transform tab. Exact LCD output appears "
        "as a 3D cross-section overlay after Slice; Run uses that prepared PWSZ.");
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
    m_grid_snap = new QCheckBox("Enabled");
    m_grid_snap->setChecked(true);
    m_snap_step = spin(0.001, 1000.0, 1.0, 3, 0.1);
    m_snap_step->setSuffix(" mm");
    zone_form->addRow("Grid snap", m_grid_snap);
    zone_form->addRow("Snap step", m_snap_step);
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
    const auto update_snap = [this] {
        m_view->setGridSnapEnabled(m_grid_snap->isChecked());
        m_view->setGridSnapStep(m_snap_step->value());
        for (QDoubleSpinBox *value : m_move)
            value->setSingleStep(m_snap_step->value());
        m_snap_step->setEnabled(m_grid_snap->isChecked());
    };
    connect(m_grid_snap, &QCheckBox::toggled, this,
            [update_snap](bool) { update_snap(); });
    connect(m_snap_step, &QDoubleSpinBox::valueChanged, this,
            [update_snap](double) { update_snap(); });
    for (QDoubleSpinBox *value : {m_layer_height, m_bottom_exposure, m_exposure,
                                  m_lift_height, m_lift_speed, m_off_time})
        connect(value, &QDoubleSpinBox::valueChanged, this,
                [this] { printSettingsChanged(); });
    connect(m_bottom_layers, &QSpinBox::valueChanged, this,
            [this] { printSettingsChanged(); });
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
    m_connection_status = new QLabel("Not checked");
    m_connection_status->setStyleSheet("font-weight: 600; color: #8e9bad;");
    form->addRow("Connection", m_connection_status);
    m_printer_id = new QLineEdit;
    form->addRow("Printer ID", m_printer_id);
    layout->addLayout(form);
    layout->addWidget(new QLabel("Printer response"));
    m_printer_response = new QPlainTextEdit;
    m_printer_response->setReadOnly(true);
    m_printer_response->setPlaceholderText("Connect / detect printer to read the cloud response.");
    m_printer_response->setMaximumHeight(155);
    layout->addWidget(m_printer_response);
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
    m_view->setBoxSelectionHandler(
        [this](const std::vector<int> &indices, Qt::KeyboardModifiers modifiers) {
            selectionFromBox(indices, modifiers);
        });
    m_view->setTransformHandler([this](PrintView::GizmoMode mode, QVector3D delta) {
        gizmoTransform(static_cast<int>(mode), delta);
    });
    m_view->setTransformFinishedHandler([this] { transformGestureFinished(); });
}

void MainWindow::loadSettings() {
    QSettings settings;
    const QByteArray geometry = settings.value("window/geometry").toByteArray();
    const QByteArray state = settings.value("window/state").toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    // Layout v2 moves Models on plate under Settings/Transform. Do not restore
    // the obsolete left-dock state once; subsequent user rearrangements persist.
    if (!state.isEmpty() && settings.value("window/layoutVersion", 0).toInt() >= 2)
        restoreState(state);
    m_zone[0]->setValue(settings.value("print/zoneX", M7_BUILD_X).toDouble());
    m_zone[1]->setValue(settings.value("print/zoneY", M7_BUILD_Y).toDouble());
    m_zone[2]->setValue(settings.value("print/zoneZ", M7_BUILD_Z).toDouble());
    m_grid_snap->setChecked(settings.value("view/gridSnap", true).toBool());
    m_snap_step->setValue(settings.value("view/snapStep", 1.0).toDouble());
    m_view->setGridSnapEnabled(m_grid_snap->isChecked());
    m_view->setGridSnapStep(m_snap_step->value());
    m_snap_step->setEnabled(m_grid_snap->isChecked());
    for (QDoubleSpinBox *value : m_move)
        value->setSingleStep(m_snap_step->value());
    const auto opacity_setting = [&settings](const char *opacity_key,
                                             const char *legacy_transparency_key) {
        if (settings.contains(opacity_key))
            return std::clamp(settings.value(opacity_key).toInt(), 0, 100);
        // Migrate the previous inverted control without changing the user's
        // saved appearance: opacity = 100 - transparency.
        return 100 - std::clamp(settings.value(legacy_transparency_key, 78).toInt(),
                                0, 100);
    };
    m_support_opacity->setValue(
        opacity_setting("view/supportOpacity", "view/supportTransparency"));
    m_model_opacity->setValue(
        opacity_setting("view/modelOpacity", "view/modelTransparency"));
    m_layer_height->setValue(settings.value("print/layerHeight", 0.05).toDouble());
    m_bottom_layers->setValue(settings.value("print/bottomLayers", 5).toInt());
    m_bottom_exposure->setValue(settings.value("print/bottomExposure", 28.0).toDouble());
    m_exposure->setValue(settings.value("print/exposure", 1.8).toDouble());
    m_lift_height->setValue(settings.value("print/liftHeight", 8.0).toDouble());
    m_lift_speed->setValue(settings.value("print/liftSpeed", 6.0).toDouble());
    m_off_time->setValue(settings.value("print/offTime", 0.5).toDouble());
    m_default_supports.enabled = settings.value("supports/enabled", false).toBool();
    m_default_supports.automatic = settings.value("supports/automatic", true).toBool();
    if (settings.value("supports/shapeVersion", 0).toInt() < 2)
        m_default_supports.shape = "branch";
    else
        m_default_supports.shape = settings.value("supports/shape", "branch").toString();
    m_default_supports.angle = settings.value("supports/angle", 45.0).toDouble();
    m_default_supports.spacing = settings.value("supports/spacing", 4.0).toDouble();
    m_default_supports.density = settings.value("supports/density", 6.25).toDouble();
    m_default_supports.tip = settings.value("supports/tip", 0.5).toDouble();
    m_default_supports.trunk = settings.value("supports/trunk", 1.2).toDouble();
    m_default_supports.base = settings.value("supports/base", 4.5).toDouble();
    m_default_supports.tip_height = settings.value("supports/tipHeight", 1.5).toDouble();
    m_default_supports.base_height = settings.value("supports/baseHeight", 0.8).toDouble();
    m_default_supports.penetration = settings.value("supports/penetration", 0.15).toDouble();
    m_default_supports.minimum_height = settings.value("supports/minHeight", 2.0).toDouble();
    m_default_supports.maximum_height = settings.value("supports/maxHeight", 180.0).toDouble();
    m_default_supports.branch_max_distance =
        settings.value("supports/branchMaxDistance", 30.0).toDouble();
    m_default_supports.branch_max_children =
        settings.value("supports/branchMaxChildren", 6).toInt();
    m_default_supports.branch_max_trunk =
        settings.value("supports/branchMaxTrunk", 3.2).toDouble();
    m_default_supports.segments = settings.value("supports/segments", 12).toInt();
    m_default_supports.trunk =
        std::max(m_default_supports.tip, m_default_supports.trunk);
    m_default_supports.base =
        std::max(m_default_supports.trunk, m_default_supports.base);
    m_default_supports.branch_max_trunk =
        std::clamp(m_default_supports.branch_max_trunk, m_default_supports.trunk,
                   m_default_supports.base);
    m_default_supports.maximum_height =
        std::max(m_default_supports.minimum_height, m_default_supports.maximum_height);
    m_default_supports.density = std::max(0.1, m_default_supports.density);
    m_default_supports.branch_max_children =
        std::clamp(m_default_supports.branch_max_children, 1, 32);
    const QJsonDocument preset_document = QJsonDocument::fromJson(
        settings.value("supports/customPresets").toByteArray());
    for (const QJsonValue &entry : preset_document.array()) {
        const QJsonObject object = entry.toObject();
        const QString name = object.value("name").toString().trimmed();
        if (!name.isEmpty() && object.value("settings").isObject())
            m_custom_support_presets.push_back(
                {name, supportFromJson(object.value("settings").toObject())});
    }
    refreshSupportPresetCombo();
    m_default_pcb.exposure = settings.value("pcb/exposure", 60.0).toDouble();
    // The GUI selector is deprecated; keep deterministic KiCad drill subtraction.
    m_default_pcb.drill_mode = "clear";
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
    settings.setValue("window/layoutVersion", 2);
    settings.setValue("print/zoneX", m_zone[0]->value());
    settings.setValue("print/zoneY", m_zone[1]->value());
    settings.setValue("print/zoneZ", m_zone[2]->value());
    settings.setValue("view/gridSnap", m_grid_snap->isChecked());
    settings.setValue("view/snapStep", m_snap_step->value());
    settings.setValue("view/supportOpacity", m_support_opacity->value());
    settings.setValue("view/modelOpacity", m_model_opacity->value());
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
    settings.setValue("supports/shapeVersion", 2);
    settings.setValue("supports/angle", m_default_supports.angle);
    settings.setValue("supports/spacing", m_default_supports.spacing);
    settings.setValue("supports/density", m_default_supports.density);
    settings.setValue("supports/tip", m_default_supports.tip);
    settings.setValue("supports/trunk", m_default_supports.trunk);
    settings.setValue("supports/base", m_default_supports.base);
    settings.setValue("supports/tipHeight", m_default_supports.tip_height);
    settings.setValue("supports/baseHeight", m_default_supports.base_height);
    settings.setValue("supports/penetration", m_default_supports.penetration);
    settings.setValue("supports/minHeight", m_default_supports.minimum_height);
    settings.setValue("supports/maxHeight", m_default_supports.maximum_height);
    settings.setValue("supports/branchMaxDistance",
                      m_default_supports.branch_max_distance);
    settings.setValue("supports/branchMaxChildren",
                      m_default_supports.branch_max_children);
    settings.setValue("supports/branchMaxTrunk", m_default_supports.branch_max_trunk);
    settings.setValue("supports/segments", m_default_supports.segments);
    QJsonArray custom_presets;
    for (const NamedSupportPreset &preset : m_custom_support_presets)
        custom_presets.append(
            QJsonObject{{"name", preset.name}, {"settings", supportJson(preset.settings)}});
    settings.setValue("supports/customPresets",
                      QJsonDocument(custom_presets).toJson(QJsonDocument::Compact));
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

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
        QTimer::singleShot(0, this, [this] { syncSupportPreviewVisibility(); });
    if (event->type() == QEvent::KeyPress && QApplication::activeWindow() == this) {
        auto *key = static_cast<QKeyEvent *>(event);
        const auto modifiers = key->modifiers() &
                               (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier |
                                Qt::MetaModifier);
        if (m_slice_overlay_active && key->key() == Qt::Key_Escape) {
            exitSliceOverlay();
            return true;
        }
        if (key->key() == Qt::Key_Z &&
            modifiers == (Qt::ControlModifier | Qt::ShiftModifier)) {
            redo();
            return true;
        }
        if (key->key() == Qt::Key_Z && modifiers == Qt::ControlModifier) {
            if (m_slice_overlay_active) {
                exitSliceOverlay();
                return true;
            }
            undo();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::importDialog() {
    importPaths(QFileDialog::getOpenFileNames(
        this, "Import objects", QString(),
        "Supported (*.stl *.3mf *.step *.stp *.iges *.igs *.gbr *.ger *.pho *.gtl *.gbl "
        "*.gts *.gbs *.gto *.gbo *.gm1 *.gko *.drl *.xln *.exc);;All files (*)"));
}

void MainWindow::importPaths(const QStringList &paths) {
    if (m_busy_depth > 0) {
        appendLog("Import ignored while another operation is running.");
        return;
    }
    std::optional<EditorSnapshot> before;
    if (m_history_enabled && !m_restoring_history)
        before = captureSnapshot();
    int imported = 0;
    for (const QString &path : paths)
        imported += importPath(path) ? 1 : 0;
    if (imported == 0)
        return;
    if (before)
        pushHistory(std::move(*before));
    for (auto &object : m_objects)
        object->selected = false;
    m_objects.back()->selected = true;
    refreshObjectList();
    refreshView();
    m_view->resetCamera();
    refreshPropertyPanels();
    if (std::count_if(m_objects.begin(), m_objects.end(), [](const auto &object) {
            return object->visible && object->isArtwork();
        }) > 1)
        rebuildArtworkPreviews();
    invalidateSlice();
    // A standalone ACV is already a sliced inspection scene. It has no
    // editable source project or runnable PWSZ, but should open in the same 3D
    // cross-section overlay used after the Slice button.
    const SceneObject &last = *m_objects.back();
    if (last.type == SceneObjectType::ViewerScene && !last.visual.layers.isEmpty()) {
        m_sliced_layers = last.visual.layers;
        m_slice_overlay_active = true;
        m_view->setLayers(m_sliced_layers);
        m_view->setSliceOverlay(true);
        m_layer_slider->setValue(0);
        updateLayerControls();
    }
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
    refreshView(false);
    refreshPropertyPanels();
    updateActions();
}

void MainWindow::selectionFromView(int index, Qt::KeyboardModifiers modifiers) {
    const bool extend = modifiers & (Qt::ControlModifier | Qt::ShiftModifier);
    const bool keep_group = !extend && index >= 0 &&
                            index < static_cast<int>(m_objects.size()) &&
                            m_objects[index]->selected;
    if (!extend && !keep_group)
        for (auto &object : m_objects)
            object->selected = false;
    if (index >= 0 && index < static_cast<int>(m_objects.size())) {
        if (extend)
            m_objects[index]->selected = !m_objects[index]->selected;
        else
            m_objects[index]->selected = true;
    }
    refreshObjectList();
    refreshView(false);
    refreshPropertyPanels();
}

void MainWindow::selectionFromBox(const std::vector<int> &indices,
                                  Qt::KeyboardModifiers modifiers) {
    const bool extend = modifiers & (Qt::ControlModifier | Qt::ShiftModifier);
    if (!extend)
        for (auto &object : m_objects)
            object->selected = false;
    for (int index : indices)
        if (index >= 0 && index < static_cast<int>(m_objects.size()))
            m_objects[index]->selected = true;
    refreshObjectList();
    refreshView(false);
    refreshPropertyPanels();
    updateActions();
}

void MainWindow::refreshObjectList() {
    m_loading_controls = true;
    m_object_list->clear();
    QListWidgetItem *first_selected = nullptr;
    for (const auto &object : m_objects) {
        auto *item = new QListWidgetItem(
            QString("[%1] %2").arg(typeName(object->type), object->display_name));
        item->setSelected(object->selected);
        if (object->selected && !first_selected)
            first_selected = item;
        item->setToolTip(object->path);
        m_object_list->addItem(item);
    }
    if (first_selected) {
        m_object_list->setCurrentItem(first_selected, QItemSelectionModel::NoUpdate);
        m_object_list->scrollToItem(first_selected);
    }
    m_loading_controls = false;
}

void MainWindow::refreshView(bool reload_geometry) {
    if (reload_geometry) {
        m_view->setObjects(m_objects);
        m_view->setLayers(m_sliced_layers);
    } else {
        m_view->updateObjectTransforms(m_objects);
    }
    const auto selected = selectedObjects();
    m_view->setRotationZOnly(!selected.empty() &&
                             std::all_of(selected.begin(), selected.end(),
                                         [](const SceneObject *object) {
                                             return object->isArtwork();
                                         }));
    float minimum_scale = std::numeric_limits<float>::max();
    for (const SceneObject *object : selected) {
        minimum_scale = std::min(minimum_scale, object->scale.x());
        minimum_scale = std::min(minimum_scale, object->scale.y());
        if (object->isModel())
            minimum_scale = std::min(minimum_scale, object->scale.z());
    }
    m_view->setMinimumSelectedScale(selected.empty() ? 1.0f : minimum_scale);
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
    if (any)
        m_tabs->setCurrentIndex(0);
    loadTransformControls();
    loadSupportControls();
    loadPcbControls();
}

void MainWindow::loadTransformControls() {
    const auto selected = selectedObjects();
    const SceneObject *object = selected.empty() ? nullptr : selected.front();
    const bool automatic_front_group = std::any_of(
        m_objects.begin(), m_objects.end(), [](const auto &candidate) {
            return candidate->visible && candidate->type == SceneObjectType::Gerber &&
                   candidate->pcb.auto_mirror &&
                   filenameSuggestsFrontMirror(candidate->path);
        });
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
                                       automatic_front_group)
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
    m_support_tip->setRange(0.05, 20.0);
    m_support_trunk->setRange(0.05, 30.0);
    m_support_base->setRange(0.05, 50.0);
    m_support_min_height->setRange(0.0, 500.0);
    m_support_max_height->setRange(0.01, 1000.0);
    m_support_branch_max_trunk->setRange(0.05, 50.0);
    const QList<QWidget *> widgets{m_support_preset,      m_support_enabled,
                                   m_support_auto,        m_support_shape,
                                   m_support_angle,       m_support_spacing,
                                   m_support_density,     m_support_tip,
                                   m_support_trunk,       m_support_base,
                                   m_support_tip_height,  m_support_base_height,
                                   m_support_penetration, m_support_min_height,
                                   m_support_max_height,  m_support_branch_max_distance,
                                   m_support_branch_max_children,
                                   m_support_branch_max_trunk, m_support_segments,
                                   m_manual_supports};
    for (QWidget *widget : widgets)
        widget->setEnabled(model != nullptr);
    if (model) {
        const SupportSettings &support = model->supports;
        m_support_preset->setCurrentIndex(supportPresetIndexFor(support));
        m_support_enabled->setChecked(support.enabled);
        m_support_auto->setChecked(support.automatic);
        m_support_shape->setCurrentText(support.shape);
        m_support_angle->setValue(support.angle);
        m_support_spacing->setValue(support.spacing);
        m_support_density->setValue(support.density);
        m_support_tip->setValue(support.tip);
        m_support_trunk->setValue(support.trunk);
        m_support_base->setValue(support.base);
        m_support_tip_height->setValue(support.tip_height);
        m_support_base_height->setValue(support.base_height);
        m_support_penetration->setValue(support.penetration);
        m_support_min_height->setValue(support.minimum_height);
        m_support_max_height->setValue(support.maximum_height);
        m_support_branch_max_distance->setValue(support.branch_max_distance);
        m_support_branch_max_children->setValue(support.branch_max_children);
        m_support_branch_max_trunk->setValue(support.branch_max_trunk);
        m_support_segments->setValue(support.segments);
    }
    refreshManualSupports();
    m_loading_controls = false;
    enforceSupportConstraints();
    updateSupportControlVisibility();
}

void MainWindow::loadPcbControls() {
    SceneObject *object = firstSelectedArtwork();
    m_loading_controls = true;
    const QList<QWidget *> pcb_widgets{m_pcb_exposure, m_pcb_invert, m_pcb_invert_margin,
                                       m_pcb_simplify};
    for (QWidget *widget : pcb_widgets)
        widget->setEnabled(object != nullptr);
    m_pcb_drill_mode->setEnabled(false);
    if (object) {
        m_pcb_exposure->setValue(object->pcb.exposure);
        m_pcb_drill_mode->setCurrentText(object->pcb.drill_mode);
        m_pcb_invert->setChecked(object->pcb.invert);
        m_pcb_invert_margin->setValue(object->pcb.invert_margin);
        m_pcb_simplify->setValue(object->pcb.simplify);
        const auto selected = selectedObjects();
        const int count = std::count_if(selected.begin(), selected.end(),
                                        [](const SceneObject *value) { return value->isArtwork(); });
        m_pcb_status->setText(
            QString("%1 PCB object(s) selected. Settings affect only them. Drill pixels is "
                    "deprecated; DRL polarity follows Invert.")
                .arg(count));
    } else {
        m_pcb_status->setText("Select a Gerber/DRL object to edit PCB settings.");
    }
    m_loading_controls = false;
}

void MainWindow::applyTransformLive() {
    if (m_loading_controls)
        return;
    recordHistory(true);
    const bool automatic_front_group = std::any_of(
        m_objects.begin(), m_objects.end(), [](const auto &candidate) {
            return candidate->visible && candidate->type == SceneObjectType::Gerber &&
                   candidate->pcb.auto_mirror &&
                   filenameSuggestsFrontMirror(candidate->path);
        });
    for (SceneObject *object : selectedObjects()) {
        if (!object->editable)
            continue;
        const QVector3D old_move = object->move;
        const QVector3D old_rotate = object->rotate;
        QVector3D old_signed_scale = object->scale;
        if (object->mirror_x ||
            (object->isArtwork() && object->pcb.auto_mirror && automatic_front_group))
            old_signed_scale.setX(-old_signed_scale.x());
        if (object->mirror_y)
            old_signed_scale.setY(-old_signed_scale.y());
        if (object->mirror_z)
            old_signed_scale.setZ(-old_signed_scale.z());

        const QVector3D new_move(m_move[0]->value(), m_move[1]->value(),
                                 m_move[2]->value());
        const QVector3D new_rotate =
            QVector3D(object->isArtwork() ? 0.0 : m_rotate[0]->value(),
                      object->isArtwork() ? 0.0 : m_rotate[1]->value(),
                      m_rotate[2]->value());
        QVector3D new_scale;
        if (object->isArtwork()) {
            const float uniform = m_scale[0]->value();
            new_scale = QVector3D(uniform, uniform, 1.0f);
        } else {
            new_scale = QVector3D(m_scale[0]->value(), m_scale[1]->value(),
                                  m_scale[2]->value());
        }
        const bool new_mirror_x = m_mirror[0]->isChecked();
        const bool new_mirror_y = m_mirror[1]->isChecked();
        const bool new_mirror_z = object->isModel() && m_mirror[2]->isChecked();
        QVector3D new_signed_scale = new_scale;
        if (new_mirror_x)
            new_signed_scale.setX(-new_signed_scale.x());
        if (new_mirror_y)
            new_signed_scale.setY(-new_signed_scale.y());
        if (new_mirror_z)
            new_signed_scale.setZ(-new_signed_scale.z());

        const QVector3D scale_ratio(new_signed_scale.x() / old_signed_scale.x(),
                                    new_signed_scale.y() / old_signed_scale.y(),
                                    new_signed_scale.z() / old_signed_scale.z());
        if ((scale_ratio - QVector3D(1.0f, 1.0f, 1.0f)).lengthSquared() > 1.0e-12f)
            transformVisual(*object, static_cast<int>(PrintView::GizmoMode::Scale),
                            scale_ratio);
        const QVector3D rotation_delta = new_rotate - old_rotate;
        if (rotation_delta.lengthSquared() > 1.0e-12f)
            transformVisual(*object, static_cast<int>(PrintView::GizmoMode::Rotate),
                            rotation_delta);
        const QVector3D move_delta = new_move - old_move;
        if (move_delta.lengthSquared() > 1.0e-12f)
            transformVisual(*object, static_cast<int>(PrintView::GizmoMode::Move), move_delta);

        object->move = new_move;
        object->rotate = new_rotate;
        object->scale = new_scale;
        object->mirror_x = new_mirror_x;
        object->mirror_y = new_mirror_y;
        object->mirror_z = new_mirror_z;
        if (object->isArtwork())
            object->pcb.auto_mirror = false;
    }
    invalidateSlice();
    m_view->updateObjectTransforms(m_objects);
}

void MainWindow::applySupportsLive() {
    updateSupportPreview();
    if (m_loading_controls)
        return;
    recordHistory(true);
    for (SceneObject *object : selectedObjects()) {
        if (!object->isModel())
            continue;
        SupportSettings &support = object->supports;
        support.enabled = m_support_enabled->isChecked();
        support.automatic = m_support_auto->isChecked();
        support.shape = m_support_shape->currentText();
        support.angle = m_support_angle->value();
        support.spacing = m_support_spacing->value();
        support.density = m_support_density->value();
        support.tip = m_support_tip->value();
        support.trunk = m_support_trunk->value();
        support.base = m_support_base->value();
        support.tip_height = m_support_tip_height->value();
        support.base_height = m_support_base_height->value();
        support.penetration = m_support_penetration->value();
        support.minimum_height = m_support_min_height->value();
        support.maximum_height = m_support_max_height->value();
        support.branch_max_distance = m_support_branch_max_distance->value();
        support.branch_max_children = m_support_branch_max_children->value();
        support.branch_max_trunk = m_support_branch_max_trunk->value();
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
    recordHistory(true);
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

void MainWindow::snapArtworkSelection() {
    std::vector<SceneObject *> selected_artwork;
    for (SceneObject *object : selectedObjects())
        if (object->isArtwork() && object->editable)
            selected_artwork.push_back(object);
    if (selected_artwork.empty()) {
        QMessageBox::information(this, "Snap PCB layers",
                                 "Select a Gerber or drill layer to use as the anchor.");
        return;
    }
    SceneObject *anchor = selected_artwork.front();
    const auto gerber = std::find_if(selected_artwork.begin(), selected_artwork.end(),
                                     [](const SceneObject *object) {
                                         return object->type == SceneObjectType::Gerber;
                                     });
    if (gerber != selected_artwork.end())
        anchor = *gerber;
    std::vector<SceneObject *> targets = selected_artwork;
    if (targets.size() == 1) {
        targets.clear();
        for (auto &object : m_objects)
            if (object->visible && object->isArtwork() && object->editable)
                targets.push_back(object.get());
    }
    if (targets.size() < 2) {
        QMessageBox::information(this, "Snap PCB layers",
                                 "Import at least two Gerber/DRL layers to align them.");
        return;
    }
    recordHistory(false);
    const bool automatic_front_group = std::any_of(
        m_objects.begin(), m_objects.end(), [](const auto &object) {
            return object->visible && object->type == SceneObjectType::Gerber &&
                   object->pcb.auto_mirror && filenameSuggestsFrontMirror(object->path);
        });
    const bool mirror_x = anchor->mirror_x ||
                          (anchor->pcb.auto_mirror && automatic_front_group);
    const QVector3D target_move = anchor->move;
    const QVector3D target_rotate(0.0f, 0.0f, anchor->rotate.z());
    const QVector3D target_scale(anchor->scale.x(), anchor->scale.x(), 1.0f);
    const bool target_mirror_y = anchor->mirror_y;
    for (SceneObject *object : targets) {
        const QVector3D old_move = object->move;
        const QVector3D old_rotate = object->rotate;
        QVector3D old_signed_scale = object->scale;
        if (object->mirror_x ||
            (object->pcb.auto_mirror && automatic_front_group))
            old_signed_scale.setX(-old_signed_scale.x());
        if (object->mirror_y)
            old_signed_scale.setY(-old_signed_scale.y());

        QVector3D target_signed_scale = target_scale;
        if (mirror_x)
            target_signed_scale.setX(-target_signed_scale.x());
        if (target_mirror_y)
            target_signed_scale.setY(-target_signed_scale.y());
        const QVector3D scale_ratio(target_signed_scale.x() / old_signed_scale.x(),
                                    target_signed_scale.y() / old_signed_scale.y(), 1.0f);
        if ((scale_ratio - QVector3D(1.0f, 1.0f, 1.0f)).lengthSquared() > 1.0e-12f)
            transformVisual(*object, static_cast<int>(PrintView::GizmoMode::Scale),
                            scale_ratio);
        const QVector3D rotation_delta = target_rotate - old_rotate;
        if (rotation_delta.lengthSquared() > 1.0e-12f)
            transformVisual(*object, static_cast<int>(PrintView::GizmoMode::Rotate),
                            rotation_delta);
        const QVector3D move_delta = target_move - old_move;
        if (move_delta.lengthSquared() > 1.0e-12f)
            transformVisual(*object, static_cast<int>(PrintView::GizmoMode::Move), move_delta);

        object->move = target_move;
        object->rotate = target_rotate;
        object->scale = target_scale;
        object->mirror_x = mirror_x;
        object->mirror_y = target_mirror_y;
        object->mirror_z = false;
        object->pcb.auto_mirror = false;
    }
    appendLog(QString("Snapped %1 PCB layers to %2.")
                  .arg(targets.size())
                  .arg(anchor->display_name));
    loadTransformControls();
    invalidateSlice();
    m_view->updateObjectTransforms(m_objects);
}

void MainWindow::applySupportPreset(int index) {
    if (m_loading_controls || index == 0)
        return;
    const int add_index = 5 + static_cast<int>(m_custom_support_presets.size());
    if (index == add_index) {
        addSupportPreset();
        return;
    }
    SupportSettings support;
    if (index >= 5 && index < add_index) {
        support = m_custom_support_presets[index - 5].settings;
    } else if (index >= 1 && index <= 4) {
        support.enabled = index != 1;
        support.automatic = true;
        support.shape = "branch";
        if (index == 2) {
            support.spacing = 5.0;
            support.density = 4.0;
            support.tip = 0.35;
            support.trunk = 0.8;
            support.base = 3.5;
            support.tip_height = 1.5;
            support.branch_max_trunk = 2.5;
        } else if (index == 3) {
            support.spacing = 4.0;
            support.density = 6.25;
            support.tip = 0.5;
            support.trunk = 1.2;
            support.base = 4.5;
            support.tip_height = 1.5;
            support.branch_max_trunk = 3.2;
        } else if (index == 4) {
            support.spacing = 3.0;
            support.density = 100.0 / 9.0;
            support.tip = 0.8;
            support.trunk = 1.8;
            support.base = 6.0;
            support.tip_height = 2.0;
            support.branch_max_trunk = 4.5;
        }
    } else {
        return;
    }
    recordHistory(false);
    for (SceneObject *object : selectedObjects())
        if (object->isModel())
            object->supports = support;
    m_default_supports = support;
    m_default_supports.manual.clear();
    applySupportSettingsToControls(support);
    refreshManualSupports();
    invalidateSlice();
    const QSignalBlocker blocker(m_support_preset);
    m_support_preset->setCurrentIndex(index);
}

void MainWindow::applySupportSettingsToControls(const SupportSettings &support) {
    m_loading_controls = true;
    // Presets may move all related values at once. Temporarily relax the live
    // editor bounds so assignment order cannot clamp an otherwise valid set.
    m_support_tip->setRange(0.05, 20.0);
    m_support_trunk->setRange(0.05, 30.0);
    m_support_base->setRange(0.05, 50.0);
    m_support_min_height->setRange(0.0, 500.0);
    m_support_max_height->setRange(0.01, 1000.0);
    m_support_branch_max_trunk->setRange(0.05, 50.0);
    m_support_enabled->setChecked(support.enabled);
    m_support_auto->setChecked(support.automatic);
    m_support_shape->setCurrentText(support.shape);
    m_support_angle->setValue(support.angle);
    m_support_spacing->setValue(support.spacing);
    m_support_density->setValue(support.density);
    m_support_tip->setValue(support.tip);
    m_support_trunk->setValue(support.trunk);
    m_support_base->setValue(support.base);
    m_support_tip_height->setValue(support.tip_height);
    m_support_base_height->setValue(support.base_height);
    m_support_penetration->setValue(support.penetration);
    m_support_min_height->setValue(support.minimum_height);
    m_support_max_height->setValue(support.maximum_height);
    m_support_branch_max_distance->setValue(support.branch_max_distance);
    m_support_branch_max_children->setValue(support.branch_max_children);
    m_support_branch_max_trunk->setValue(support.branch_max_trunk);
    m_support_segments->setValue(support.segments);
    m_loading_controls = false;
    enforceSupportConstraints();
    updateSupportControlVisibility();
    updateSupportPreview();
}

void MainWindow::enforceSupportConstraints(QWidget *changed) {
    if (!m_support_tip || (m_loading_controls && changed))
        return;
    const bool previous_loading = m_loading_controls;
    m_loading_controls = true;

    // Resolve programmatic transitions first, then publish hard spin-box
    // bounds. Keyboard entry and mouse-wheel stepping can no longer construct
    // tip > branch > base or min-height > max-height configurations.
    if (changed == m_support_tip && m_support_tip->value() > m_support_trunk->value())
        m_support_trunk->setValue(m_support_tip->value());
    if (changed == m_support_trunk) {
        if (m_support_tip->value() > m_support_trunk->value())
            m_support_tip->setValue(m_support_trunk->value());
        if (m_support_trunk->value() > m_support_base->value())
            m_support_base->setValue(m_support_trunk->value());
    }
    if (changed == m_support_base && m_support_trunk->value() > m_support_base->value()) {
        m_support_trunk->setValue(m_support_base->value());
        m_support_tip->setValue(std::min(m_support_tip->value(), m_support_trunk->value()));
    }
    if (m_support_branch_max_trunk->value() < m_support_trunk->value())
        m_support_branch_max_trunk->setValue(m_support_trunk->value());
    if (m_support_branch_max_trunk->value() > m_support_base->value())
        m_support_branch_max_trunk->setValue(m_support_base->value());
    if (changed == m_support_min_height &&
        m_support_min_height->value() > m_support_max_height->value())
        m_support_max_height->setValue(m_support_min_height->value());
    if (changed == m_support_max_height &&
        m_support_max_height->value() < m_support_min_height->value())
        m_support_min_height->setValue(m_support_max_height->value());

    m_support_tip->setRange(0.05, m_support_trunk->value());
    m_support_trunk->setRange(m_support_tip->value(), m_support_base->value());
    m_support_base->setRange(m_support_trunk->value(), 50.0);
    m_support_branch_max_trunk->setRange(m_support_trunk->value(),
                                          m_support_base->value());
    m_support_min_height->setRange(0.0, m_support_max_height->value());
    m_support_max_height->setRange(std::max(0.01, m_support_min_height->value()), 1000.0);
    m_loading_controls = previous_loading;
    updateSupportPreview();
}

void MainWindow::updateSupportControlVisibility() {
    if (!m_support_form || !m_support_shape)
        return;
    const bool branch = m_support_shape->currentText() == "branch";
    for (QWidget *field : {static_cast<QWidget *>(m_support_angle),
                           static_cast<QWidget *>(m_support_branch_max_distance),
                           static_cast<QWidget *>(m_support_branch_max_children),
                           static_cast<QWidget *>(m_support_branch_max_trunk)}) {
        field->setVisible(branch);
        if (QWidget *label = m_support_form->labelForField(field))
            label->setVisible(branch);
    }
}

void MainWindow::updateSupportPreview() {
    if (!m_support_preview || !m_support_enabled)
        return;
    SupportSettings support;
    support.enabled = m_support_enabled->isChecked();
    support.automatic = m_support_auto->isChecked();
    support.shape = m_support_shape->currentText();
    support.angle = m_support_angle->value();
    support.spacing = m_support_spacing->value();
    support.density = m_support_density->value();
    support.tip = m_support_tip->value();
    support.trunk = m_support_trunk->value();
    support.base = m_support_base->value();
    support.tip_height = m_support_tip_height->value();
    support.base_height = m_support_base_height->value();
    support.penetration = m_support_penetration->value();
    support.minimum_height = m_support_min_height->value();
    support.maximum_height = m_support_max_height->value();
    support.branch_max_distance = m_support_branch_max_distance->value();
    support.branch_max_children = m_support_branch_max_children->value();
    support.branch_max_trunk = m_support_branch_max_trunk->value();
    support.segments = m_support_segments->value();
    m_support_preview->setSettings(support);
}

QString MainWindow::supportPreviewLabel(QWidget *editor) const {
    const std::array<std::pair<QWidget *, const char *>, 19> labels{{
        {m_support_preset, "Preset"},
        {m_support_enabled, "Enabled"},
        {m_support_auto, "Automatic placement"},
        {m_support_shape, "Support shape"},
        {m_support_angle, "Overhang angle"},
        {m_support_spacing, "Contact spacing"},
        {m_support_density, "Contact density"},
        {m_support_tip, "Tip diameter"},
        {m_support_trunk, "Trunk diameter"},
        {m_support_base, "Base diameter"},
        {m_support_tip_height, "Tip height"},
        {m_support_base_height, "Base height"},
        {m_support_penetration, "Contact penetration"},
        {m_support_min_height, "Minimum support height"},
        {m_support_max_height, "Maximum support height"},
        {m_support_branch_max_distance, "Tree reach zone"},
        {m_support_branch_max_children, "Maximum branches per node"},
        {m_support_branch_max_trunk, "Maximum shared trunk diameter"},
        {m_support_segments, "Radial segments"},
    }};
    for (const auto &[control, label] : labels)
        if (control == editor)
            return QString::fromLatin1(label);
    return QStringLiteral("Support geometry");
}

void MainWindow::syncSupportPreviewVisibility() {
    if (!m_support_preview)
        return;
    QWidget *focused = QApplication::focusWidget();
    QWidget *editor = nullptr;
    for (QWidget *control : m_support_preview_controls) {
        if (focused && (focused == control || control->isAncestorOf(focused))) {
            editor = control;
            break;
        }
    }
    if (!editor || !isVisible() || !m_support_panel || !m_support_panel->isVisible()) {
        m_support_preview->hide();
        return;
    }

    showSupportPreviewFor(editor);
}

void MainWindow::showSupportPreviewFor(QWidget *editor) {
    updateSupportPreview();
    m_support_preview->setFocusedParameter(supportPreviewLabel(editor));
    QPoint position = editor->mapToGlobal(QPoint(editor->width() + 10, 0));
    QScreen *screen = editor->screen();
    const QRect available = screen ? screen->availableGeometry() : QRect();
    if (available.isValid()) {
        if (position.x() + m_support_preview->width() > available.right() + 1)
            position.setX(editor->mapToGlobal(
                                     QPoint(-m_support_preview->width() - 10, 0))
                              .x());
        position.setX(std::clamp(position.x(), available.left(),
                                 available.right() - m_support_preview->width() + 1));
        position.setY(std::clamp(position.y(), available.top(),
                                 available.bottom() - m_support_preview->height() + 1));
    }
    m_support_preview->move(position);
    m_support_preview->show();
    m_support_preview->raise();
}

int MainWindow::supportPresetIndexFor(const SupportSettings &support) const {
    const int built_in = supportPresetIndex(support);
    if (built_in != 0)
        return built_in;
    for (int index = 0; index < static_cast<int>(m_custom_support_presets.size()); ++index)
        if (sameSupportSettings(support, m_custom_support_presets[index].settings))
            return index + 5;
    return 0;
}

void MainWindow::refreshSupportPresetCombo(const QString &selected_name) {
    QString wanted = selected_name;
    if (wanted.isEmpty() && m_support_preset)
        wanted = m_support_preset->currentText();
    const QSignalBlocker blocker(m_support_preset);
    m_support_preset->clear();
    m_support_preset->addItems({"Custom", "None", "Light", "Medium", "Heavy"});
    for (const NamedSupportPreset &preset : m_custom_support_presets)
        m_support_preset->addItem(preset.name);
    m_support_preset->addItem("+ Add new preset");
    const int found = m_support_preset->findText(wanted, Qt::MatchFixedString);
    m_support_preset->setCurrentIndex(found >= 0 ? found : 0);
}

void MainWindow::addSupportPreset() {
    SceneObject *model = firstSelectedModel();
    if (!model) {
        refreshSupportPresetCombo();
        return;
    }
    bool accepted = false;
    QString name = QInputDialog::getText(this, "New support preset", "Preset name:",
                                         QLineEdit::Normal, {}, &accepted)
                       .trimmed();
    if (!accepted || name.isEmpty()) {
        const QSignalBlocker blocker(m_support_preset);
        m_support_preset->setCurrentIndex(supportPresetIndexFor(model->supports));
        return;
    }
    const bool duplicate = std::any_of(
        m_custom_support_presets.begin(), m_custom_support_presets.end(),
        [&name](const NamedSupportPreset &preset) {
            return preset.name.compare(name, Qt::CaseInsensitive) == 0;
        });
    if (duplicate || m_support_preset->findText(name, Qt::MatchFixedString) >= 0) {
        QMessageBox::warning(this, "New support preset",
                             "A preset with this name already exists.");
        const QSignalBlocker blocker(m_support_preset);
        m_support_preset->setCurrentIndex(supportPresetIndexFor(model->supports));
        return;
    }
    m_custom_support_presets.push_back({name, model->supports});
    refreshSupportPresetCombo(name);
    saveSettings();
    appendLog("Saved support preset: " + name);
}

void MainWindow::deleteCurrentSupportPreset() {
    const int custom_index = m_support_preset->currentIndex() - 5;
    if (custom_index < 0 ||
        custom_index >= static_cast<int>(m_custom_support_presets.size()))
        return;
    const QString name = m_custom_support_presets[custom_index].name;
    m_custom_support_presets.erase(m_custom_support_presets.begin() + custom_index);
    refreshSupportPresetCombo();
    if (SceneObject *model = firstSelectedModel())
        m_support_preset->setCurrentIndex(supportPresetIndexFor(model->supports));
    saveSettings();
    appendLog("Deleted support preset: " + name);
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
    recordHistory(false);
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
    recordHistory(false);
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
    if (m_busy_depth > 0)
        return;
    const bool removing_artwork = std::any_of(
        m_objects.begin(), m_objects.end(),
        [](const auto &object) { return object->selected && object->isArtwork(); });
    const auto old_size = m_objects.size();
    if (!std::any_of(m_objects.begin(), m_objects.end(),
                     [](const auto &object) { return object->selected; }))
        return;
    recordHistory(false);
    m_objects.erase(std::remove_if(m_objects.begin(), m_objects.end(),
                                   [](const auto &object) { return object->selected; }),
                    m_objects.end());
    if (m_objects.size() == old_size)
        return;
    refreshObjectList();
    refreshView();
    refreshPropertyPanels();
    if (removing_artwork)
        rebuildArtworkPreviews();
    invalidateSlice();
}

void MainWindow::autoLayout() {
    if (m_busy_depth > 0)
        return;
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
    std::vector<QVector3D> deltas;
    deltas.reserve(objects.size());
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
        deltas.push_back(target - current);
        cursor_x += width + margin;
        row_depth = std::max(row_depth, depth);
    }
    recordHistory(false);
    for (std::size_t index = 0; index < objects.size(); ++index) {
        SceneObject *object = objects[index];
        const QVector3D delta = deltas[index];
        object->move += delta;
        transformVisual(*object, static_cast<int>(PrintView::GizmoMode::Move), delta);
    }
    loadTransformControls();
    invalidateSlice();
    m_view->updateObjectTransforms(m_objects);
}

void MainWindow::gizmoTransform(int mode, const QVector3D &delta) {
    if (m_busy_depth > 0)
        return;
    if (!m_gizmo_history_recorded) {
        recordHistory(false);
        m_gizmo_history_recorded = true;
    }
    for (SceneObject *object : selectedObjects()) {
        if (!object->editable)
            continue;
        QVector3D applied = delta;
        if (mode == static_cast<int>(PrintView::GizmoMode::Move)) {
            if (object->isArtwork() && object->move.z() + applied.z() < 0.0f)
                applied.setZ(-object->move.z());
            object->move += applied;
        } else if (mode == static_cast<int>(PrintView::GizmoMode::Rotate)) {
            if (object->isArtwork())
                applied = QVector3D(0.0f, 0.0f, applied.z());
            object->rotate += applied;
            object->rotate.setX(std::remainder(object->rotate.x(), 360.0f));
            object->rotate.setY(std::remainder(object->rotate.y(), 360.0f));
            object->rotate.setZ(std::remainder(object->rotate.z(), 360.0f));
        } else {
            const QVector3D previous = object->scale;
            QVector3D next(std::max(1.0f, std::round(previous.x() + applied.x())),
                           std::max(1.0f, std::round(previous.y() + applied.y())),
                           std::max(1.0f, std::round(previous.z() + applied.z())));
            if (object->isArtwork())
                next = QVector3D(next.x(), next.x(), 1.0f);
            object->scale = next;
            applied = QVector3D(next.x() / previous.x(), next.y() / previous.y(),
                                next.z() / previous.z());
        }
        transformVisual(*object, mode, applied);
    }
    loadTransformControls();
    invalidateSlice();
    m_view->updateObjectTransforms(m_objects);
}

void MainWindow::transformGestureFinished() {
    m_gizmo_history_recorded = false;
}

void MainWindow::transformVisual(SceneObject &object, int mode, const QVector3D &delta) {
    const QVector3D centre = (object.visual.minimum + object.visual.maximum) * 0.5f;
    QMatrix4x4 matrix;
    if (mode == static_cast<int>(PrintView::GizmoMode::Move)) {
        matrix.translate(delta);
    } else {
        matrix.translate(centre);
        if (mode == static_cast<int>(PrintView::GizmoMode::Rotate)) {
            matrix.rotate(delta.x(), 1.0f, 0.0f, 0.0f);
            matrix.rotate(delta.y(), 0.0f, 1.0f, 0.0f);
            matrix.rotate(delta.z(), 0.0f, 0.0f, 1.0f);
        } else {
            matrix.scale(delta);
        }
        matrix.translate(-centre);
    }
    object.display_transform = matrix * object.display_transform;

    // Updating the eight AABB corners is constant-time. The millions of model
    // vertices remain untouched in their VBO and are transformed by the shader.
    const QVector3D old_minimum = object.visual.minimum;
    const QVector3D old_maximum = object.visual.maximum;
    QVector3D minimum(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max());
    QVector3D maximum = -minimum;
    const auto include = [&](const QVector3D &point) {
        const QVector3D transformed = matrix.map(point);
        minimum.setX(std::min(minimum.x(), transformed.x()));
        minimum.setY(std::min(minimum.y(), transformed.y()));
        minimum.setZ(std::min(minimum.z(), transformed.z()));
        maximum.setX(std::max(maximum.x(), transformed.x()));
        maximum.setY(std::max(maximum.y(), transformed.y()));
        maximum.setZ(std::max(maximum.z(), transformed.z()));
    };
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z)
                include(QVector3D(x ? old_maximum.x() : old_minimum.x(),
                                  y ? old_maximum.y() : old_minimum.y(),
                                  z ? old_maximum.z() : old_minimum.z()));
    object.visual.minimum = minimum;
    object.visual.maximum = maximum;
}

void MainWindow::rebuildArtworkPreviews() {
    for (auto &object : m_objects)
        if (object->editable && object->visible && object->isArtwork() &&
            !rebuildObject(*object, false))
            appendLog("PCB preview rebuild failed for " + object->display_name);
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
    const QString operation = object.isArtwork() ? "project-object-viewer-scene"
                                                 : "project-model-viewer-scene";
    QStringList arguments{operation, project_path};
    arguments.append("0");
    arguments.append(scene_path);
    const ProcessResult result = runRust(arguments, "Preparing " + object.display_name,
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
    object.display_transform.setToIdentity();
    return true;
}

void MainWindow::updateLayerControls() {
    const int count = m_view->layerCount();
    const int maximum = std::max(0, count - 1);
    const bool sliced = m_slice_overlay_active && count > 0;
    if (m_layer_slider->minimum() != 0 || m_layer_slider->maximum() != maximum)
        m_layer_slider->setRange(0, maximum);
    m_layer_slider->setEnabled(sliced);
    {
        const QSignalBlocker blocker(m_layer_number);
        m_layer_number->setRange(1, std::max(1, count));
        m_layer_number->setValue(std::clamp(m_layer_slider->value() + 1, 1,
                                            std::max(1, count)));
    }
    m_layer_number->setEnabled(sliced);
    m_support_opacity->setEnabled(sliced);
    m_model_opacity->setEnabled(sliced);
    updateLayerLabel(m_layer_slider->value());
}

void MainWindow::updateLayerLabel(int layer) {
    if (m_view->layerCount() == 0) {
        m_layer_label->setText("/ 0, Z=0.000 mm");
        return;
    }
    m_layer_label->setText(QString("/ %1, Z=%2 mm")
                               .arg(m_view->layerCount())
                               .arg(m_view->layerZ(layer), 0, 'f', 3));
}

void MainWindow::exitSliceOverlay() {
    if (!m_slice_overlay_active)
        return;
    m_slice_overlay_active = false;
    m_view->setSliceOverlay(false);
    updateLayerControls();
    appendLog("Exited sliced-layer overlay. The prepared PWSZ remains available.");
}

void MainWindow::printingZoneChanged() {
    if (m_restoring_history)
        return;
    if (!m_view || !m_zone[0])
        return;
    recordPrintSettingsHistory();
    m_view->setBuildVolume(
        QVector3D(m_zone[0]->value(), m_zone[1]->value(), m_zone[2]->value()));
    invalidateSlice();
}

void MainWindow::printSettingsChanged() {
    if (m_restoring_history)
        return;
    recordPrintSettingsHistory();
    invalidateSlice();
}

void MainWindow::recordPrintSettingsHistory() {
    if (!m_history_enabled || m_restoring_history) {
        syncPrintSettingsHistoryBaseline();
        return;
    }
    if (!m_history_coalescing) {
        EditorSnapshot before = captureSnapshot();
        before.zone = m_history_zone;
        before.layer_height = m_history_layer_height;
        before.bottom_layers = m_history_bottom_layers;
        before.bottom_exposure = m_history_bottom_exposure;
        before.exposure = m_history_exposure;
        before.lift_height = m_history_lift_height;
        before.lift_speed = m_history_lift_speed;
        before.off_time = m_history_off_time;
        pushHistory(std::move(before));
        m_history_coalescing = true;
    }
    m_history_timer->start();
    syncPrintSettingsHistoryBaseline();
}

void MainWindow::syncPrintSettingsHistoryBaseline() {
    if (!m_zone[0])
        return;
    for (int axis = 0; axis < 3; ++axis)
        m_history_zone[axis] = m_zone[axis]->value();
    m_history_layer_height = m_layer_height->value();
    m_history_bottom_layers = m_bottom_layers->value();
    m_history_bottom_exposure = m_bottom_exposure->value();
    m_history_exposure = m_exposure->value();
    m_history_lift_height = m_lift_height->value();
    m_history_lift_speed = m_lift_speed->value();
    m_history_off_time = m_off_time->value();
}

void MainWindow::invalidateSlice() {
    const bool had_layer_geometry = !m_sliced_layers.isEmpty() || m_slice_overlay_active;
    const bool had_prepared_job = !m_last_pwsz.isEmpty() || m_cloud_file_id.has_value();
    if (!m_last_pwsz.isEmpty())
        appendLog("Scene changed: prepared PWSZ invalidated.");
    m_last_pwsz.clear();
    m_cloud_file_id.reset();
    m_sliced_layers.clear();
    m_slice_overlay_active = false;
    if (m_view) {
        m_view->setSlicedSupports({});
        if (had_layer_geometry) {
            m_view->setLayers({});
            m_view->setSliceOverlay(false);
        }
    }
    if (m_output_label && (had_prepared_job || had_layer_geometry))
        m_output_label->setText("No prepared PWSZ. Press Slice.");
    if (m_cloud_file_label && had_prepared_job)
        m_cloud_file_label->setText("Cloud file: not uploaded");
    if (had_layer_geometry)
        updateLayerControls();
    updateActions();
}

void MainWindow::updateActions() {
    if (!m_slice_action)
        return;
    const bool sliceable = std::any_of(m_objects.begin(), m_objects.end(), [](const auto &object) {
        return object->editable && object->visible;
    });
    const bool idle = m_busy_depth == 0;
    m_import_action->setEnabled(idle);
    m_layout_action->setEnabled(idle && sliceable);
    m_slice_action->setEnabled(idle && sliceable);
    m_run_action->setEnabled(idle && !m_last_pwsz.isEmpty());
    m_pause_action->setEnabled(idle && !m_printer_id->text().trimmed().isEmpty());
    m_resume_action->setEnabled(m_pause_action->isEnabled());
    m_stop_action->setEnabled(m_pause_action->isEnabled());
    m_status_action->setEnabled(idle && QFileInfo(m_cloud_config->text()).isFile());
    updateHistoryActions();
}

bool MainWindow::sliceScene() {
    if (m_busy_depth > 0)
        return false;
    if (m_objects.empty())
        return false;
    QString suggested = QDir::current().filePath("plate.pwsz");
    for (const auto &object : m_objects) {
        if (object->editable) {
            suggested = QFileInfo(object->path).absoluteDir().filePath("plate.pwsz");
            break;
        }
    }
    QString output = m_slice_output_path;
    if (output.isEmpty()) {
        output = QFileDialog::getSaveFileName(this, "Write sliced job", suggested,
                                              "Anycubic print job (*.pwsz)");
        if (output.isEmpty())
            return false;
        if (QFileInfo(output).suffix().isEmpty())
            output += ".pwsz";
        m_slice_output_path = QFileInfo(output).absoluteFilePath();
    }
    QString project_path;
    QString error;
    if (!writeTemporaryProject(project_path, error)) {
        QMessageBox::critical(this, "Slice", error);
        return false;
    }
    const QString scene_path = makeTemporaryPath(".acv");
    const ProcessResult sliced =
        runRust({"project-slice-with-viewer", project_path, output, scene_path},
                "Slicing unified scene", 60 * 60 * 1000);
    if (!sliced.ok) {
        QFile::remove(project_path);
        QFile::remove(scene_path);
        QMessageBox::critical(this, "Slice", sliced.output);
        return false;
    }
    QFile::remove(project_path);
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
    m_view->setSlicedSupports(scene.support_vertices);
    m_view->setLayers(m_sliced_layers);
    m_slice_overlay_active = !m_sliced_layers.isEmpty();
    m_view->setSliceOverlay(m_slice_overlay_active);
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
    if (!QFileInfo(m_cloud_config->text()).isFile()) {
        m_connection_status->setText("Fail");
        m_connection_status->setStyleSheet("font-weight: 700; color: #e66b6b;");
        m_printer_response->setPlainText("Account config does not exist or is not a file.");
        return false;
    }
    const ProcessResult status = runRust({"cloud-status", m_cloud_config->text()},
                                         "Reading Cloud printers", 60 * 1000);
    m_printer_response->setPlainText(status.output);
    if (!status.ok) {
        m_connection_status->setText("Fail");
        m_connection_status->setStyleSheet("font-weight: 700; color: #e66b6b;");
        if (show_result)
            QMessageBox::critical(this, "Cloud", status.output);
        return false;
    }
    QRegularExpression expression("id=Some\\(\\\"?(\\d+)\\\"?\\)");
    auto match = expression.match(status.output);
    if (!match.hasMatch()) {
        expression.setPattern("id=(\\d+)");
        match = expression.match(status.output);
    }
    if (!match.hasMatch()) {
        m_connection_status->setText("Fail");
        m_connection_status->setStyleSheet("font-weight: 700; color: #e66b6b;");
        return false;
    }
    if (m_printer_id->text().trimmed().isEmpty())
        m_printer_id->setText(match.captured(1));
    saveSettings();
    updateActions();
    m_connection_status->setText("Success");
    m_connection_status->setStyleSheet("font-weight: 700; color: #61c98b;");
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
    ++m_busy_depth;
    updateActions();
    const auto busy_guard = qScopeGuard([this] {
        m_busy_depth = std::max(0, m_busy_depth - 1);
        updateActions();
    });
    const QString executable = rustCli();
    if (executable.isEmpty())
        return {false, "Could not locate the anycubicstudio Rust CLI."};
    QStringList safe = arguments;
    if (!safe.isEmpty() && safe.front() == "cloud-login-complete")
        safe.append("<code via stdin>");
    appendLog("$ " + executable + " " + safe.join(' '));
    QProgressDialog progress(title, "Cancel", 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.show();
    // Paint the busy state before process creation/import work begins. Without
    // this explicit event turn Qt showed no feedback during CLI startup.
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(executable, arguments);
    if (!process.waitForStarted(5000))
        return {false, process.errorString()};
    if (!standard_input.isEmpty()) {
        process.write(standard_input);
        process.closeWriteChannel();
    }
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
    const bool automatic_front_group =
        (isolated && isolated->type == SceneObjectType::Gerber &&
         isolated->pcb.auto_mirror && filenameSuggestsFrontMirror(isolated->path)) ||
        std::any_of(m_objects.begin(), m_objects.end(), [](const auto &candidate) {
            return candidate->visible && candidate->type == SceneObjectType::Gerber &&
                   candidate->pcb.auto_mirror &&
                   filenameSuggestsFrontMirror(candidate->path);
        });
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
        const SupportSettings &preview_supports = isolated ? disabled : object->supports;
        objects.append(QJsonObject{{"input", object->path},
                                   {"source_type", sourceTypeJson(object->type)},
                                   {"visible", true},
                                   {"transform", transformJson(*object, automatic_front_group)},
                                   {"supports", supportJson(preview_supports)},
                                   {"pcb", pcb}});
    };
    if (isolated) {
        // A newly imported object is previewed before ownership is transferred
        // to m_objects, so it must be serialized directly rather than found in
        // the current scene collection.
        append_object(isolated);
        if (isolated->isArtwork())
            for (const auto &owned : m_objects)
                if (owned.get() != isolated && owned->visible && owned->isArtwork())
                    append_object(owned.get());
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

MainWindow::EditorSnapshot MainWindow::captureSnapshot() const {
    EditorSnapshot snapshot;
    snapshot.objects.reserve(m_objects.size());
    for (const auto &object : m_objects)
        snapshot.objects.push_back(*object);
    snapshot.default_supports = m_default_supports;
    snapshot.default_pcb = m_default_pcb;
    for (int axis = 0; axis < 3; ++axis)
        snapshot.zone[axis] = m_zone[axis]->value();
    snapshot.layer_height = m_layer_height->value();
    snapshot.bottom_layers = m_bottom_layers->value();
    snapshot.bottom_exposure = m_bottom_exposure->value();
    snapshot.exposure = m_exposure->value();
    snapshot.lift_height = m_lift_height->value();
    snapshot.lift_speed = m_lift_speed->value();
    snapshot.off_time = m_off_time->value();
    return snapshot;
}

void MainWindow::restoreSnapshot(const EditorSnapshot &snapshot) {
    m_restoring_history = true;
    m_loading_controls = true;
    m_objects.clear();
    m_objects.reserve(snapshot.objects.size());
    quint64 maximum_id = 0;
    for (const SceneObject &object : snapshot.objects) {
        m_objects.push_back(std::make_unique<SceneObject>(object));
        maximum_id = std::max(maximum_id, object.id);
    }
    m_next_object_id = maximum_id + 1;
    m_default_supports = snapshot.default_supports;
    m_default_pcb = snapshot.default_pcb;
    for (int axis = 0; axis < 3; ++axis)
        m_zone[axis]->setValue(snapshot.zone[axis]);
    m_layer_height->setValue(snapshot.layer_height);
    m_bottom_layers->setValue(snapshot.bottom_layers);
    m_bottom_exposure->setValue(snapshot.bottom_exposure);
    m_exposure->setValue(snapshot.exposure);
    m_lift_height->setValue(snapshot.lift_height);
    m_lift_speed->setValue(snapshot.lift_speed);
    m_off_time->setValue(snapshot.off_time);
    m_loading_controls = false;
    m_restoring_history = false;
    syncPrintSettingsHistoryBaseline();
    m_view->setBuildVolume(
        QVector3D(snapshot.zone[0], snapshot.zone[1], snapshot.zone[2]));
    refreshObjectList();
    refreshView();
    refreshPropertyPanels();
    invalidateSlice();
}

void MainWindow::pushHistory(EditorSnapshot snapshot) {
    if (!m_history_enabled || m_restoring_history)
        return;
    m_history_coalescing = false;
    if (m_history_timer)
        m_history_timer->stop();
    m_undo_history.push_back(std::move(snapshot));
    // Scene previews can contain millions of contour/vertex values; keep a
    // bounded but useful history instead of allowing snapshots to exhaust RAM.
    if (m_undo_history.size() > 20)
        m_undo_history.erase(m_undo_history.begin());
    m_redo_history.clear();
    updateHistoryActions();
}

void MainWindow::recordHistory(bool coalesce) {
    if (!m_history_enabled || m_restoring_history)
        return;
    if (coalesce && m_history_coalescing) {
        m_history_timer->start();
        return;
    }
    pushHistory(captureSnapshot());
    if (coalesce) {
        m_history_coalescing = true;
        m_history_timer->start();
    }
}

void MainWindow::undo() {
    if (m_busy_depth > 0 || m_undo_history.empty())
        return;
    m_history_coalescing = false;
    m_history_timer->stop();
    m_redo_history.push_back(captureSnapshot());
    EditorSnapshot snapshot = std::move(m_undo_history.back());
    m_undo_history.pop_back();
    restoreSnapshot(snapshot);
    updateHistoryActions();
}

void MainWindow::redo() {
    if (m_busy_depth > 0 || m_redo_history.empty())
        return;
    m_history_coalescing = false;
    m_history_timer->stop();
    m_undo_history.push_back(captureSnapshot());
    EditorSnapshot snapshot = std::move(m_redo_history.back());
    m_redo_history.pop_back();
    restoreSnapshot(snapshot);
    updateHistoryActions();
}

void MainWindow::updateHistoryActions() {
    if (m_undo_action)
        m_undo_action->setEnabled(m_busy_depth == 0 && !m_undo_history.empty());
    if (m_redo_action)
        m_redo_action->setEnabled(m_busy_depth == 0 && !m_redo_history.empty());
}

bool MainWindow::runHistorySmokeTest(QString &error) {
    SceneObject *object = nullptr;
    for (auto &candidate : m_objects)
        if (candidate->editable) {
            object = candidate.get();
            break;
        }
    if (!object) {
        error = "history smoke test needs one editable scene object";
        return false;
    }
    for (auto &candidate : m_objects)
        candidate->selected = candidate.get() == object;
    refreshObjectList();
    refreshPropertyPanels();

    const QVector3D original_minimum = object->visual.minimum;
    const QVector3D original_maximum = object->visual.maximum;
    const float original_first_vertex = object->visual.model_vertices.isEmpty()
                                            ? 0.0f
                                            : object->visual.model_vertices.front();
    transformVisual(*object, static_cast<int>(PrintView::GizmoMode::Move),
                    QVector3D(1.0f, 0.0f, 0.0f));
    if ((!object->visual.model_vertices.isEmpty() &&
         object->visual.model_vertices.front() != original_first_vertex) ||
        std::abs(object->visual.minimum.x() - (original_minimum.x() + 1.0f)) > 1.0e-5f) {
        error = "GPU transform path rewrote model vertices or failed to update bounds";
        return false;
    }
    transformVisual(*object, static_cast<int>(PrintView::GizmoMode::Move),
                    QVector3D(-1.0f, 0.0f, 0.0f));
    if ((object->visual.minimum - original_minimum).length() > 1.0e-4f ||
        (object->visual.maximum - original_maximum).length() > 1.0e-4f) {
        error = "GPU transform round-trip did not restore bounds";
        return false;
    }
    object->display_transform.setToIdentity();
    m_view->updateObjectTransforms(m_objects);

    const float original_x = object->move.x();
    m_move[0]->setValue(original_x + 1.0f);
    m_history_coalescing = false;
    m_history_timer->stop();
    undo();
    if (m_objects.empty() || std::abs(m_objects.front()->move.x() - original_x) > 1.0e-5f) {
        error = "Ctrl+Z transform state was not restored";
        return false;
    }
    redo();
    if (m_objects.empty() ||
        std::abs(m_objects.front()->move.x() - (original_x + 1.0f)) > 1.0e-5f) {
        error = "Ctrl+Shift+Z transform state was not restored";
        return false;
    }
    const double original_exposure = m_exposure->value();
    m_exposure->setValue(original_exposure + 0.1);
    m_history_coalescing = false;
    m_history_timer->stop();
    undo();
    if (std::abs(m_exposure->value() - original_exposure) > 1.0e-6) {
        error = "Ctrl+Z print setting was not restored";
        return false;
    }
    redo();
    if (std::abs(m_exposure->value() - (original_exposure + 0.1)) > 1.0e-6) {
        error = "Ctrl+Shift+Z print setting was not restored";
        return false;
    }
    return true;
}

bool MainWindow::runSupportPreviewSmokeTest(QString &error) {
    SceneObject *model = nullptr;
    for (auto &candidate : m_objects) {
        candidate->selected = false;
        if (!model && candidate->isModel())
            model = candidate.get();
    }
    if (!model) {
        error = "support preview smoke test needs one imported model";
        return false;
    }
    model->selected = true;
    refreshObjectList();
    refreshPropertyPanels();
    m_tabs->setCurrentWidget(m_support_panel);
    // The headless Qt platform cannot create QOpenGLWidget contexts. The
    // popup itself is a lightweight QPainter widget, so keep the 3D canvas
    // explicitly hidden while exercising the popup and repaint lifecycle.
    m_view->hide();
    setAttribute(Qt::WA_DontShowOnScreen, true);
    show();
    QApplication::processEvents();

    showSupportPreviewFor(m_support_tip);
    QApplication::processEvents();
    if (!m_support_preview->isVisible()) {
        error = "preview did not enter its visible lifecycle state";
        hide();
        return false;
    }
    const double original = m_support_tip->value();
    m_support_tip->setValue(std::min(m_support_tip->maximum(), original + 0.05));
    QApplication::processEvents();
    const QPixmap rendered = m_support_preview->grab();
    if (rendered.isNull() || rendered.size() != m_support_preview->size()) {
        error = "preview did not render a valid frame after a live value change";
        hide();
        return false;
    }

    m_support_preview->hide();
    QApplication::processEvents();
    if (m_support_preview->isVisible()) {
        error = "preview did not leave its visible lifecycle state";
        hide();
        return false;
    }
    hide();
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
