#include "main_window.hpp"

#include "print_view.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
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
#include <QSet>
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

namespace {

QDoubleSpinBox *spin(double minimum, double maximum, double value, int decimals = 3,
                     double step = 1.0) {
    auto *result = new QDoubleSpinBox;
    result->setRange(minimum, maximum);
    result->setDecimals(decimals);
    result->setSingleStep(step);
    result->setValue(value);
    result->setKeyboardTracking(false);
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

QJsonObject transformJson(const ModelState &model) {
    QVector3D scale = model.scale;
    if (model.mirror_x)
        scale.setX(-scale.x());
    if (model.mirror_y)
        scale.setY(-scale.y());
    if (model.mirror_z)
        scale.setZ(-scale.z());
    return {{"translation_mm", tripleJson(model.move)},
            {"rotation_deg", tripleJson(model.rotate)},
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

QString quoted(const QString &value) {
    if (!value.contains(QRegularExpression("[\\s\\\"']")))
        return value;
    QString escaped = value;
    escaped.replace('"', "\\\"");
    return '"' + escaped + '"';
}

bool filenameSuggestsFrontMirror(const QString &path) {
    QString normalized;
    const QString name = QFileInfo(path).fileName().toLower();
    normalized.reserve(name.size());
    for (const QChar character : name) {
        if (character.isLetterOrNumber())
            normalized.append(character);
        else if (!normalized.endsWith('_'))
            normalized.append('_');
    }
    const QString padded = QStringLiteral("_") + normalized + QStringLiteral("_");
    for (const QString &marker : {QStringLiteral("f_cu"), QStringLiteral("f_mask"),
                                  QStringLiteral("f_paste"),
                                  QStringLiteral("f_silkscreen")}) {
        if (padded.contains(QStringLiteral("_") + marker + QStringLiteral("_")))
            return true;
    }
    return false;
}

} // namespace

MainWindow::MainWindow(const QStringList &initial_files, QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("Anycubic Studio — plate and printer control");
    resize(1500, 900);
    setAcceptDrops(true);
    buildInterface();
    connectActions();
    loadSettings();
    printingZoneChanged();
    if (!initial_files.isEmpty())
        importPaths(initial_files);
    updateActions();
    if (QFileInfo(m_cloud_config->text()).isFile()) {
        QTimer::singleShot(0, this, [this] { connectToCloud(false); });
    }
}

void MainWindow::buildInterface() {
    auto *toolbar = addToolBar("Workflow");
    toolbar->setObjectName("workflowToolbar");
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->setMovable(false);
    QAction *import_action = toolbar->addAction("Import…");
    m_remove_action = toolbar->addAction("Remove");
    m_layout_action = toolbar->addAction("Auto layout");
    toolbar->addSeparator();
    m_slice_action = toolbar->addAction("Slice & Run…");
    toolbar->addSeparator();
    m_pause_action = toolbar->addAction("Pause");
    m_resume_action = toolbar->addAction("Resume");
    m_stop_action = toolbar->addAction("Stop");
    m_status_action = toolbar->addAction("Printer status");

    connect(import_action, &QAction::triggered, this, [this] { importDialog(); });
    connect(m_remove_action, &QAction::triggered, this, [this] { removeSelected(); });
    connect(m_layout_action, &QAction::triggered, this, [this] { autoLayout(); });
    connect(m_slice_action, &QAction::triggered, this, [this] { sliceAndRun(); });
    connect(m_pause_action, &QAction::triggered, this, [this] { pausePrinting(); });
    connect(m_resume_action, &QAction::triggered, this, [this] { resumePrinting(); });
    connect(m_stop_action, &QAction::triggered, this, [this] { stopPrinting(); });
    connect(m_status_action, &QAction::triggered, this, [this] { requestPrinterStatus(); });

    auto *splitter = new QSplitter;
    auto *model_panel = new QWidget;
    auto *model_layout = new QVBoxLayout(model_panel);
    model_layout->addWidget(new QLabel("Models on plate"));
    m_model_list = new QListWidget;
    m_model_list->setMinimumWidth(210);
    model_layout->addWidget(m_model_list, 1);
    auto *import_hint = new QLabel(
        "Use the same Import… action or drag-and-drop for STL/3MF/STEP/IGES and "
        "Gerber/DRL. The input type selects the slicing pipeline automatically.");
    import_hint->setWordWrap(true);
    import_hint->setStyleSheet("color: #8e9bad;");
    model_layout->addWidget(import_hint);
    splitter->addWidget(model_panel);
    splitter->addWidget(buildCentre());

    auto *tabs = new QTabWidget;
    tabs->setMinimumWidth(355);
    tabs->addTab(buildTransformPanel(), "Transform");
    tabs->addTab(buildSupportPanel(), "Supports");
    tabs->addTab(buildPrintPanel(), "Print & zone");
    tabs->addTab(buildPcbPanel(), "PCB exposure");
    tabs->addTab(buildCloudPanel(), "Printer/Cloud");
    splitter->addWidget(tabs);
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
    dock->setMinimumHeight(130);
    addDockWidget(Qt::BottomDockWidgetArea, dock);

    appendLog("GUI ready. Import is unified; Slice & Run selects the 3D or PCB pipeline.");
}

QWidget *MainWindow::buildCentre() {
    auto *container = new QWidget;
    auto *layout = new QVBoxLayout(container);
    auto *controls = new QHBoxLayout;
    m_view_mode = new QComboBox;
    m_view_mode->addItems({"3D model & supports", "Sliced layer"});
    m_show_model = new QCheckBox("Model");
    m_show_model->setChecked(true);
    m_show_supports = new QCheckBox("Supports");
    m_show_supports->setChecked(true);
    auto *reset = new QPushButton("Reset camera");
    controls->addWidget(m_view_mode);
    controls->addWidget(m_show_model);
    controls->addWidget(m_show_supports);
    controls->addStretch();
    controls->addWidget(reset);
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
        "Camera: left drag = orbit, Shift+left / middle / right drag = pan, wheel = zoom. "
        "Selected model is orange.");
    help->setWordWrap(true);
    help->setStyleSheet("color: #8e9bad;");
    layout->addWidget(help);

    connect(reset, &QPushButton::clicked, m_view, &PrintView::resetCamera);
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
    auto *apply = new QPushButton("Apply transform");
    auto *centre = new QPushButton("Center X/Y");
    auto *bed = new QPushButton("Place on bed");
    auto *reset = new QPushButton("Reset transform");
    layout->addWidget(apply);
    layout->addWidget(rowOf({centre, bed}));
    layout->addWidget(reset);
    auto *note = new QLabel(
        "Transforms are evaluated by the Rust slicer. Mirror uses a signed per-axis scale and "
        "repairs triangle winding.");
    note->setWordWrap(true);
    note->setStyleSheet("color: #8e9bad;");
    layout->addWidget(note);
    layout->addStretch();
    connect(apply, &QPushButton::clicked, this, [this] { applyTransform(); });
    connect(centre, &QPushButton::clicked, this, [this] { centreSelected(); });
    connect(bed, &QPushButton::clicked, this, [this] { placeSelectedOnBed(); });
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
    m_support_auto->setChecked(true);
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
    m_support_segments->setValue(12);
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
    m_manual_supports->setMinimumHeight(95);
    layout->addWidget(m_manual_supports);
    auto *add = new QPushButton("Add X,Y,Z[,BASE_Z]");
    auto *remove = new QPushButton("Remove point");
    layout->addWidget(rowOf({add, remove}));
    auto *apply = new QPushButton("Apply supports and rebuild");
    layout->addWidget(apply);
    auto *warning = new QLabel(
        "Current independent slicer generates vertical pillar supports. Shape, diameters, "
        "heights, spacing, overhang angle and manual contact points are configurable. Tree "
        "supports and interactive ray picking are not yet equivalent to Anycubic VoxSDK.");
    warning->setWordWrap(true);
    warning->setStyleSheet("color: #c89a55;");
    layout->addWidget(warning);
    layout->addStretch();

    connect(m_support_preset, &QComboBox::activated, this,
            [this](int index) { applySupportPreset(index); });
    connect(add, &QPushButton::clicked, this, [this] { addManualSupport(); });
    connect(remove, &QPushButton::clicked, this, [this] { removeManualSupport(); });
    connect(apply, &QPushButton::clicked, this, [this] { applySupports(); });

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    return scroll;
}

QWidget *MainWindow::buildPrintPanel() {
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    auto *zone_box = new QGroupBox("Printing zone");
    auto *zone_form = new QFormLayout(zone_box);
    m_zone[0] = spin(1.0, 2000.0, M7_BUILD_X);
    m_zone[1] = spin(1.0, 2000.0, M7_BUILD_Y);
    m_zone[2] = spin(1.0, 2000.0, M7_BUILD_Z);
    zone_form->addRow("X/Y/Z (mm)", rowOf({m_zone[0], m_zone[1], m_zone[2]}));
    auto *profile = new QLabel("Default: Anycubic Photon Mono M7 Pro, 13312 × 5120 pixels.");
    profile->setWordWrap(true);
    zone_form->addRow(profile);
    layout->addWidget(zone_box);

    auto *print_box = new QGroupBox("Exposure and motion");
    auto *form = new QFormLayout(print_box);
    m_layer_height = spin(0.001, 1.0, 0.05, 3, 0.01);
    m_bottom_layers = new QSpinBox;
    m_bottom_layers->setRange(0, 1000);
    m_bottom_layers->setValue(5);
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
    layout->addWidget(print_box);
    m_output_label = new QLabel("No current PWSZ. Press Slice & Run to create one.");
    m_output_label->setWordWrap(true);
    layout->addWidget(m_output_label);
    auto *note = new QLabel(
        "Any change to geometry, supports, zone or print profile invalidates the previous "
        "sliced output. Slice & Run always rebuilds it before upload.");
    note->setWordWrap(true);
    note->setStyleSheet("color: #8e9bad;");
    layout->addWidget(note);
    layout->addStretch();

    for (QDoubleSpinBox *zone : m_zone)
        connect(zone, &QDoubleSpinBox::valueChanged, this,
                [this] { printingZoneChanged(); });
    const QList<QDoubleSpinBox *> doubles{m_layer_height,    m_bottom_exposure,
                                          m_exposure,        m_lift_height,
                                          m_lift_speed,      m_off_time};
    for (QDoubleSpinBox *value : doubles)
        connect(value, &QDoubleSpinBox::valueChanged, this,
                [this] { invalidateSlice(); });
    connect(m_bottom_layers, &QSpinBox::valueChanged, this,
            [this] { invalidateSlice(); });
    return content;
}

QWidget *MainWindow::buildPcbPanel() {
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);

    auto *files_box = new QGroupBox("KiCad production files");
    auto *files_layout = new QVBoxLayout(files_box);
    m_pcb_files = new QListWidget;
    m_pcb_files->setMinimumHeight(120);
    files_layout->addWidget(m_pcb_files);
    auto *remove = new QPushButton("Remove selected");
    files_layout->addWidget(remove);
    auto *files_note = new QLabel(
        "Import Gerber/DRL through the same top Import… action used for 3D models, or drop "
        "them anywhere in this window. Gerber and Excellon retain the same KiCad coordinate "
        "system, so drill holes are overlaid without bounding-box fitting.");
    files_note->setWordWrap(true);
    files_note->setStyleSheet("color: #8e9bad;");
    files_layout->addWidget(files_note);
    layout->addWidget(files_box);

    auto *settings_box = new QGroupBox("One-layer UV job");
    auto *form = new QFormLayout(settings_box);
    m_pcb_exposure = spin(0.01, 3600.0, 60.0, 2, 1.0);
    m_pcb_drill_mode = new QComboBox;
    m_pcb_drill_mode->addItems({"clear", "expose", "ignore"});
    m_pcb_invert = new QCheckBox("Invert inside artwork bounds");
    m_pcb_auto_mirror = new QCheckBox("Auto from KiCad F_* filename");
    m_pcb_auto_mirror->setChecked(true);
    m_pcb_mirror_x = new QCheckBox("Mirror layers (X)");
    m_pcb_mirror_x->setToolTip(
        "Passed to the Rust CLI as --mirror-layers. Automatically enabled for F_Cu, F_Mask, "
        "F_Paste and F_Silkscreen filenames; it can still be changed manually.");
    m_pcb_mirror_y = new QCheckBox("Mirror Y (advanced)");
    m_pcb_rotate = spin(-360.0, 360.0, 0.0, 2, 5.0);
    m_pcb_scale = spin(0.001, 1000.0, 1.0, 5, 0.01);
    m_pcb_move[0] = spin(-1000.0, 1000.0, 0.0);
    m_pcb_move[1] = spin(-1000.0, 1000.0, 0.0);
    m_pcb_invert_margin = spin(0.0, 1000.0, 0.0);
    m_pcb_simplify = spin(0.0, 10.0, 0.012, 4, 0.002);
    m_pcb_lift_height = spin(0.0, 100.0, 0.0, 3, 0.1);
    form->addRow("Exposure (s)", m_pcb_exposure);
    form->addRow("Drill pixels", m_pcb_drill_mode);
    form->addRow("Polarity", m_pcb_invert);
    form->addRow("Mirror detection", m_pcb_auto_mirror);
    form->addRow("Transfer orientation", rowOf({m_pcb_mirror_x, m_pcb_mirror_y}));
    form->addRow("Rotate (°)", m_pcb_rotate);
    form->addRow("Scale", m_pcb_scale);
    form->addRow("Move X/Y (mm)", rowOf({m_pcb_move[0], m_pcb_move[1]}));
    form->addRow("Invert margin (mm)", m_pcb_invert_margin);
    form->addRow("Contour simplify (mm)", m_pcb_simplify);
    form->addRow("Lift height (mm)", m_pcb_lift_height);
    layout->addWidget(settings_box);

    auto *preview = new QPushButton("Preview exact LCD layer");
    layout->addWidget(preview);
    auto *start_uploaded = new QPushButton("Start uploaded PCB (platform removed)…");
    layout->addWidget(start_uploaded);
    m_pcb_status = new QLabel("No PCB artwork loaded.");
    m_pcb_status->setWordWrap(true);
    layout->addWidget(m_pcb_status);
    auto *warning = new QLabel(
        "The result is a normal one-layer printer job, not a raw UV-lamp command. Even with "
        "zero configured lift, printer firmware may home or move the platform at print start. "
        "Front-side KiCad layer names enable Mirror layers automatically. Slice & Upload will "
        "not start the printer. Preview polarity and orientation, then remove the platform "
        "before using the explicit PCB start button.");
    warning->setWordWrap(true);
    warning->setStyleSheet("color: #d27b68;");
    layout->addWidget(warning);
    layout->addStretch();

    connect(remove, &QPushButton::clicked, this, [this] { removeSelectedPcbFile(); });
    connect(preview, &QPushButton::clicked, this, [this] { previewPcb(); });
    connect(start_uploaded, &QPushButton::clicked, this, [this] {
        if (!m_last_job_is_pcb || !m_cloud_file_id) {
            QMessageBox::warning(this, "PCB start",
                                 "Slice & Upload a PCB job in this session first.");
            return;
        }
        startPrinting();
    });
    const QList<QDoubleSpinBox *> numbers{m_pcb_exposure,      m_pcb_rotate,
                                          m_pcb_scale,         m_pcb_move[0],
                                          m_pcb_move[1],       m_pcb_invert_margin,
                                          m_pcb_simplify,      m_pcb_lift_height};
    for (QDoubleSpinBox *value : numbers)
        connect(value, &QDoubleSpinBox::valueChanged, this, [this] { invalidateSlice(); });
    connect(m_pcb_drill_mode, &QComboBox::currentIndexChanged, this,
            [this] { invalidateSlice(); });
    connect(m_pcb_auto_mirror, &QCheckBox::toggled, this, [this] {
        updatePcbAutoMirror();
        invalidateSlice();
    });
    for (QCheckBox *value : {m_pcb_invert, m_pcb_mirror_x, m_pcb_mirror_y})
        connect(value, &QCheckBox::toggled, this, [this] { invalidateSlice(); });
    return content;
}

QWidget *MainWindow::buildCloudPanel() {
    auto *panel = new QWidget;
    auto *layout = new QVBoxLayout(panel);
    auto *form = new QFormLayout;
    m_cloud_config = new QLineEdit;
    m_cloud_config->setPlaceholderText("Anycubic Photon Workshop global_config.ini");
    const QString config_from_environment = qEnvironmentVariable("ANYCUBIC_CONFIG");
    if (!config_from_environment.isEmpty())
        m_cloud_config->setText(config_from_environment);
    auto *browse = new QPushButton("Browse…");
    form->addRow("Account config", rowOf({m_cloud_config, browse}));
    auto *login = new QPushButton("Log in to Anycubic…");
    form->addRow("OAuth account", login);
    auto *connect_cloud = new QPushButton("Connect / detect printer");
    form->addRow("Cloud connection", connect_cloud);
    m_printer_id = new QLineEdit;
    m_printer_id->setPlaceholderText("Numeric cloud printer ID");
    form->addRow("Printer ID", m_printer_id);
    layout->addLayout(form);
    m_cloud_file_label = new QLabel("Cloud file: not uploaded");
    m_cloud_file_label->setWordWrap(true);
    layout->addWidget(m_cloud_file_label);
    auto *status = new QLabel(
        "Workflow: Import → Slice. A 3D job can continue to the guarded print start. A PCB job "
        "stops after upload and must be started with its separate platform-removed button. "
        "Account path, printer ID and viewer settings persist across restarts.");
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *warning = new QLabel(
        "Run/Pause/Resume/Stop control real hardware. Run and Stop require confirmation. Never "
        "commit global_config.ini: it contains an account access token.");
    warning->setWordWrap(true);
    warning->setStyleSheet("color: #d27b68;");
    layout->addWidget(warning);

    auto *exposure_group = new QGroupBox("Built-in LCD test (no print/Z command)");
    auto *exposure_layout = new QFormLayout(exposure_group);
    m_exposure_pattern = new QComboBox;
    m_exposure_pattern->addItem("Pattern 1 (raw protocol value 1)", 1);
    m_exposure_pattern->addItem("Pattern 2 (raw protocol value 2)", 2);
    m_exposure_pattern->addItem("Pattern 3 (raw protocol value 4)", 4);
    m_exposure_duration = new QSpinBox;
    m_exposure_duration->setRange(1, 300);
    m_exposure_duration->setValue(5);
    m_exposure_duration->setSuffix(" s");
    auto *start_exposure = new QPushButton("Run built-in LCD test…");
    auto *stop_exposure = new QPushButton("STOP LCD test");
    exposure_layout->addRow("Firmware pattern", m_exposure_pattern);
    exposure_layout->addRow("Duration", m_exposure_duration);
    exposure_layout->addRow(rowOf({start_exposure, stop_exposure}));
    auto *exposure_note = new QLabel(
        "This uses Workshop's separate exposure/start command. It contains no Z-axis or print "
        "project operation, but it can show only one of three firmware patterns—not the imported "
        "Gerber image.");
    exposure_note->setWordWrap(true);
    exposure_layout->addRow(exposure_note);
    layout->addWidget(exposure_group);
    layout->addStretch();
    connect(browse, &QPushButton::clicked, this, [this] { browseCloudConfig(); });
    connect(login, &QPushButton::clicked, this, [this] { loginToCloud(); });
    connect(connect_cloud, &QPushButton::clicked, this,
            [this] { connectToCloud(true); });
    connect(start_exposure, &QPushButton::clicked, this,
            [this] { startBuiltinExposure(); });
    connect(stop_exposure, &QPushButton::clicked, this, [this] { stopBuiltinExposure(); });
    return panel;
}

void MainWindow::connectActions() {
    connect(m_model_list, &QListWidget::currentRowChanged, this,
            [this](int row) { selectModel(row); });
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

    m_pcb_exposure->setValue(settings.value("pcb/exposure", 60.0).toDouble());
    m_pcb_drill_mode->setCurrentText(settings.value("pcb/drillMode", "clear").toString());
    m_pcb_invert->setChecked(settings.value("pcb/invert", false).toBool());
    m_pcb_auto_mirror->setChecked(settings.value("pcb/autoMirror", true).toBool());
    m_pcb_mirror_x->setChecked(settings.value("pcb/mirrorLayers", false).toBool());
    m_pcb_mirror_y->setChecked(settings.value("pcb/mirrorY", false).toBool());
    m_pcb_rotate->setValue(settings.value("pcb/rotate", 0.0).toDouble());
    m_pcb_scale->setValue(settings.value("pcb/scale", 1.0).toDouble());
    m_pcb_move[0]->setValue(settings.value("pcb/moveX", 0.0).toDouble());
    m_pcb_move[1]->setValue(settings.value("pcb/moveY", 0.0).toDouble());
    m_pcb_invert_margin->setValue(settings.value("pcb/invertMargin", 0.0).toDouble());
    m_pcb_simplify->setValue(settings.value("pcb/simplify", 0.012).toDouble());
    m_pcb_lift_height->setValue(settings.value("pcb/liftHeight", 0.0).toDouble());

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

    const QString configured_path = settings.value("cloud/configPath").toString();
    if (m_cloud_config->text().isEmpty() && !configured_path.isEmpty())
        m_cloud_config->setText(configured_path);
    if (m_cloud_config->text().isEmpty()) {
        const QStringList candidates{
            QDir::current().filePath("global_config.ini"),
            QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../global_config.ini")};
        for (const QString &candidate : candidates) {
            if (QFileInfo(candidate).isFile()) {
                m_cloud_config->setText(QFileInfo(candidate).absoluteFilePath());
                break;
            }
        }
    }
    m_printer_id->setText(settings.value("cloud/printerId").toString());
    m_exposure_pattern->setCurrentIndex(settings.value("cloud/exposurePattern", 0).toInt());
    m_exposure_duration->setValue(settings.value("cloud/exposureDuration", 5).toInt());
    appendLog("Restored viewer preferences from " + settings.fileName() +
              ". Print files and Cloud file IDs are intentionally not restored.");
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

    settings.setValue("pcb/exposure", m_pcb_exposure->value());
    settings.setValue("pcb/drillMode", m_pcb_drill_mode->currentText());
    settings.setValue("pcb/invert", m_pcb_invert->isChecked());
    settings.setValue("pcb/autoMirror", m_pcb_auto_mirror->isChecked());
    settings.setValue("pcb/mirrorLayers", m_pcb_mirror_x->isChecked());
    settings.setValue("pcb/mirrorY", m_pcb_mirror_y->isChecked());
    settings.setValue("pcb/rotate", m_pcb_rotate->value());
    settings.setValue("pcb/scale", m_pcb_scale->value());
    settings.setValue("pcb/moveX", m_pcb_move[0]->value());
    settings.setValue("pcb/moveY", m_pcb_move[1]->value());
    settings.setValue("pcb/invertMargin", m_pcb_invert_margin->value());
    settings.setValue("pcb/simplify", m_pcb_simplify->value());
    settings.setValue("pcb/liftHeight", m_pcb_lift_height->value());

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
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    }
    importPaths(paths);
    event->acceptProposedAction();
}

void MainWindow::importDialog() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, "Import 3D model or PCB artwork", QString(),
        "Supported inputs (*.stl *.3mf *.step *.stp *.iges *.igs *.gbr *.ger *.pho *.gtl "
        "*.gbl *.gts *.gbs *.gto *.gbo *.gm1 *.gko *.drl *.xln *.exc);;"
        "3D models (*.stl *.3mf *.step *.stp *.iges *.igs);;"
        "Gerber and drill (*.gbr *.ger *.pho *.gtl *.gbl *.gts *.gbs *.gto *.gbo *.gm1 "
        "*.gko *.drl *.xln *.exc);;All files (*)");
    importPaths(files);
}

void MainWindow::importPaths(const QStringList &paths) {
    int imported = 0;
    QStringList artwork;
    for (const QString &path : paths) {
        if (supportedArtwork(path))
            artwork.append(path);
        else
            imported += importPath(path) ? 1 : 0;
    }
    if (!artwork.isEmpty())
        importPcbPaths(artwork);
    if (imported > 0) {
        refreshModelList();
        m_model_list->setCurrentRow(static_cast<int>(m_models.size()) - 1);
        refreshView();
        invalidateSlice();
        const bool has_editable = std::any_of(
            m_models.begin(), m_models.end(), [](const auto &model) { return model->editable; });
        if (!has_editable && m_models.size() == 1) {
            m_sliced_layers = m_models.front()->visual.layers;
            m_view->setLayers(m_sliced_layers);
            updateLayerControls();
        }
    }
}

void MainWindow::updatePcbAutoMirror() {
    if (!m_pcb_auto_mirror || !m_pcb_auto_mirror->isChecked())
        return;
    const bool front_layer = std::any_of(m_pcb_gerbers.cbegin(), m_pcb_gerbers.cend(),
                                         filenameSuggestsFrontMirror);
    const bool changed = m_pcb_mirror_x->isChecked() != front_layer;
    m_pcb_mirror_x->setChecked(front_layer);
    if (changed) {
        appendLog(front_layer
                      ? "Enabled Mirror layers (X): a front-side KiCad filename was detected."
                      : "Disabled Mirror layers (X): the current set has no named front layer.");
    }
}

void MainWindow::importPcbPaths(const QStringList &paths) {
    int imported = 0;
    for (const QString &source : paths) {
        const QFileInfo info(source);
        if (!info.isFile()) {
            QMessageBox::warning(this, "PCB import", "File does not exist:\n" + source);
            continue;
        }
        const QString path = info.absoluteFilePath();
        QStringList *destination = nullptr;
        if (supportedDrill(path))
            destination = &m_pcb_drills;
        else if (supportedGerber(path))
            destination = &m_pcb_gerbers;
        else {
            QMessageBox::warning(this, "PCB import", "Unsupported artwork format:\n" + path);
            continue;
        }
        if (!destination->contains(path)) {
            destination->append(path);
            ++imported;
            appendLog("Imported PCB artwork " + path);
        }
    }
    if (imported == 0)
        return;
    updatePcbAutoMirror();
    refreshPcbFiles();
    invalidateSlice();
    m_pcb_status->setText(
        QString("Loaded %1 Gerber and %2 drill file(s). Press Preview to inspect the exact "
                "composited LCD layer.")
            .arg(m_pcb_gerbers.size())
            .arg(m_pcb_drills.size()));
}

void MainWindow::removeSelectedPcbFile() {
    const int row = m_pcb_files ? m_pcb_files->currentRow() : -1;
    if (row < 0)
        return;
    if (row < m_pcb_gerbers.size())
        m_pcb_gerbers.removeAt(row);
    else {
        const int drill = row - m_pcb_gerbers.size();
        if (drill >= 0 && drill < m_pcb_drills.size())
            m_pcb_drills.removeAt(drill);
    }
    updatePcbAutoMirror();
    refreshPcbFiles();
    invalidateSlice();
    if (m_pcb_gerbers.isEmpty())
        m_pcb_status->setText("No Gerber artwork loaded.");
}

void MainWindow::refreshPcbFiles() {
    if (!m_pcb_files)
        return;
    m_pcb_files->clear();
    for (const QString &path : m_pcb_gerbers)
        m_pcb_files->addItem("Gerber  " + QFileInfo(path).fileName());
    for (const QString &path : m_pcb_drills)
        m_pcb_files->addItem("Drill   " + QFileInfo(path).fileName());
}

bool MainWindow::importPath(const QString &source) {
    const QFileInfo info(source);
    if (!info.isFile()) {
        QMessageBox::warning(this, "Import", "File does not exist:\n" + source);
        return false;
    }
    const QString path = info.absoluteFilePath();
    auto model = std::make_unique<ModelState>();
    model->path = path;
    model->display_name = info.fileName();
    if (info.suffix().compare("acv", Qt::CaseInsensitive) == 0) {
        QString error;
        if (!loadScene(path, model->visual, error)) {
            QMessageBox::critical(this, "Import", "Cannot read viewer scene:\n" + error);
            return false;
        }
        model->editable = false;
        model->display_name += " (viewer scene)";
    } else {
        if (!supportedModel(path)) {
            QMessageBox::warning(this, "Import", "Unsupported model format:\n" + path);
            return false;
        }
        model->supports = m_default_supports;
        if (!rebuildModel(*model))
            return false;
    }
    m_models.push_back(std::move(model));
    appendLog("Imported " + path);
    return true;
}

void MainWindow::removeSelected() {
    const int row = m_model_list->currentRow();
    if (row < 0 || row >= static_cast<int>(m_models.size()))
        return;
    m_models.erase(m_models.begin() + row);
    refreshModelList();
    if (!m_models.empty())
        m_model_list->setCurrentRow(std::min(row, static_cast<int>(m_models.size()) - 1));
    else
        loadModelControls();
    refreshView();
    invalidateSlice();
}

void MainWindow::selectModel(int) {
    loadModelControls();
    refreshView();
    updateActions();
}

ModelState *MainWindow::selectedModel() {
    const int row = m_model_list ? m_model_list->currentRow() : -1;
    return row >= 0 && row < static_cast<int>(m_models.size()) ? m_models[row].get() : nullptr;
}

const ModelState *MainWindow::selectedModel() const {
    const int row = m_model_list ? m_model_list->currentRow() : -1;
    return row >= 0 && row < static_cast<int>(m_models.size()) ? m_models[row].get() : nullptr;
}

void MainWindow::refreshModelList() {
    const int selected = m_model_list->currentRow();
    m_model_list->blockSignals(true);
    m_model_list->clear();
    for (const auto &model : m_models)
        m_model_list->addItem(model->display_name);
    m_model_list->blockSignals(false);
    if (!m_models.empty())
        m_model_list->setCurrentRow(std::clamp(selected, 0, static_cast<int>(m_models.size()) - 1));
}

void MainWindow::refreshView() {
    m_view->setModelScenes(m_models, m_model_list->currentRow());
    if (!m_sliced_layers.isEmpty())
        m_view->setLayers(m_sliced_layers);
    updateLayerControls();
}

void MainWindow::loadModelControls() {
    ModelState *model = selectedModel();
    m_loading_controls = true;
    const bool enabled = model && model->editable;
    for (int axis = 0; axis < 3; ++axis) {
        m_move[axis]->setEnabled(enabled);
        m_rotate[axis]->setEnabled(enabled);
        m_scale[axis]->setEnabled(enabled);
        m_mirror[axis]->setEnabled(enabled);
        m_move[axis]->setValue(model ? model->move[axis] : 0.0);
        m_rotate[axis]->setValue(model ? model->rotate[axis] : 0.0);
        m_scale[axis]->setValue(model ? model->scale[axis] : 1.0);
    }
    if (model) {
        m_support_preset->setCurrentIndex(0);
        m_mirror[0]->setChecked(model->mirror_x);
        m_mirror[1]->setChecked(model->mirror_y);
        m_mirror[2]->setChecked(model->mirror_z);
        const SupportSettings &support = model->supports;
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
    } else {
        for (QCheckBox *mirror : m_mirror)
            mirror->setChecked(false);
    }
    const QList<QWidget *> support_widgets{
        m_support_preset,      m_support_enabled,    m_support_auto,
        m_support_shape,       m_support_angle,      m_support_spacing,
        m_support_tip,         m_support_trunk,      m_support_base,
        m_support_tip_height,  m_support_base_height,m_support_penetration,
        m_support_min_height,  m_support_max_height, m_support_segments,
        m_manual_supports};
    for (QWidget *widget : support_widgets)
        widget->setEnabled(enabled);
    refreshManualSupports();
    m_loading_controls = false;
}

void MainWindow::applyTransform() {
    ModelState *model = selectedModel();
    if (!model || !model->editable)
        return;
    ModelState previous = *model;
    model->move = QVector3D(m_move[0]->value(), m_move[1]->value(), m_move[2]->value());
    model->rotate =
        QVector3D(m_rotate[0]->value(), m_rotate[1]->value(), m_rotate[2]->value());
    model->scale = QVector3D(m_scale[0]->value(), m_scale[1]->value(), m_scale[2]->value());
    model->mirror_x = m_mirror[0]->isChecked();
    model->mirror_y = m_mirror[1]->isChecked();
    model->mirror_z = m_mirror[2]->isChecked();
    if (!rebuildModel(*model)) {
        *model = std::move(previous);
        loadModelControls();
        return;
    }
    refreshView();
    invalidateSlice();
}

void MainWindow::resetTransform() {
    for (int axis = 0; axis < 3; ++axis) {
        m_move[axis]->setValue(0.0);
        m_rotate[axis]->setValue(0.0);
        m_scale[axis]->setValue(1.0);
        m_mirror[axis]->setChecked(false);
    }
    applyTransform();
}

void MainWindow::centreSelected() {
    m_move[0]->setValue(0.0);
    m_move[1]->setValue(0.0);
    applyTransform();
}

void MainWindow::placeSelectedOnBed() {
    m_move[2]->setValue(0.0);
    applyTransform();
}

void MainWindow::autoLayout() {
    std::vector<ModelState *> editable;
    for (auto &model : m_models) {
        if (model->editable)
            editable.push_back(model.get());
    }
    if (editable.empty())
        return;
    const double margin = 5.0;
    const double min_x = -m_zone[0]->value() * 0.5 + margin;
    const double max_x = m_zone[0]->value() * 0.5 - margin;
    const double min_y = -m_zone[1]->value() * 0.5 + margin;
    const double max_y = m_zone[1]->value() * 0.5 - margin;
    double cursor_x = min_x;
    double cursor_y = min_y;
    double row_depth = 0.0;
    std::vector<QVector3D> placements;
    for (ModelState *model : editable) {
        const double width = model->visual.maximum.x() - model->visual.minimum.x();
        const double depth = model->visual.maximum.y() - model->visual.minimum.y();
        if (width + 2.0 * margin > m_zone[0]->value() ||
            depth + 2.0 * margin > m_zone[1]->value()) {
            QMessageBox::warning(this, "Auto layout",
                                 model->display_name + " is larger than the printing zone.");
            return;
        }
        if (cursor_x + width > max_x) {
            cursor_x = min_x;
            cursor_y += row_depth + margin;
            row_depth = 0.0;
        }
        if (cursor_y + depth > max_y) {
            QMessageBox::warning(this, "Auto layout",
                                 "All models do not fit into the configured printing zone.");
            return;
        }
        placements.emplace_back(cursor_x + width * 0.5, cursor_y + depth * 0.5, 0.0);
        cursor_x += width + margin;
        row_depth = std::max(row_depth, depth);
    }
    for (std::size_t index = 0; index < editable.size(); ++index) {
        editable[index]->move = placements[index];
        if (!rebuildModel(*editable[index]))
            return;
    }
    loadModelControls();
    refreshView();
    invalidateSlice();
}

void MainWindow::applySupports() {
    ModelState *model = selectedModel();
    if (!model || !model->editable)
        return;
    ModelState previous = *model;
    SupportSettings &support = model->supports;
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
    if (!rebuildModel(*model)) {
        *model = std::move(previous);
        loadModelControls();
        return;
    }
    m_default_supports = support;
    m_default_supports.manual.clear();
    m_support_preset->setCurrentIndex(0);
    refreshView();
    invalidateSlice();
}

void MainWindow::applySupportPreset(int index) {
    if (m_loading_controls || index == 0)
        return;
    if (index == 1) {
        m_support_enabled->setChecked(false);
        return;
    }
    m_support_enabled->setChecked(true);
    m_support_auto->setChecked(true);
    m_support_shape->setCurrentText("tapered");
    m_support_angle->setValue(45.0);
    m_support_tip_height->setValue(index == 4 ? 2.0 : 1.5);
    m_support_base_height->setValue(0.8);
    m_support_penetration->setValue(0.15);
    m_support_min_height->setValue(2.0);
    m_support_max_height->setValue(180.0);
    m_support_segments->setValue(12);
    if (index == 2) {
        m_support_spacing->setValue(5.0);
        m_support_tip->setValue(0.35);
        m_support_trunk->setValue(0.8);
        m_support_base->setValue(3.5);
    } else if (index == 3) {
        m_support_spacing->setValue(4.0);
        m_support_tip->setValue(0.5);
        m_support_trunk->setValue(1.2);
        m_support_base->setValue(4.5);
    } else {
        m_support_spacing->setValue(3.0);
        m_support_tip->setValue(0.8);
        m_support_trunk->setValue(1.8);
        m_support_base->setValue(6.0);
    }
}

void MainWindow::addManualSupport() {
    ModelState *model = selectedModel();
    if (!model || !model->editable)
        return;
    bool accepted = false;
    const QString value = QInputDialog::getText(
        this, "Manual support", "Contact X,Y,Z[,BASE_Z] in millimetres:",
        QLineEdit::Normal, QString(), &accepted);
    if (!accepted)
        return;
    const QStringList fields = value.split(',', Qt::SkipEmptyParts);
    if (fields.size() != 3 && fields.size() != 4) {
        QMessageBox::warning(this, "Manual support", "Enter three or four comma-separated numbers.");
        return;
    }
    double numbers[4]{};
    for (int index = 0; index < fields.size(); ++index) {
        bool ok = false;
        numbers[index] = fields[index].trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(numbers[index])) {
            QMessageBox::warning(this, "Manual support", "Invalid numeric coordinate.");
            return;
        }
    }
    ManualSupport point;
    point.contact = QVector3D(numbers[0], numbers[1], numbers[2]);
    if (fields.size() == 4)
        point.base_z = numbers[3];
    model->supports.manual.push_back(point);
    m_support_enabled->setChecked(true);
    refreshManualSupports();
    applySupports();
}

void MainWindow::removeManualSupport() {
    ModelState *model = selectedModel();
    const int row = m_manual_supports->currentRow();
    if (!model || row < 0 || row >= static_cast<int>(model->supports.manual.size()))
        return;
    model->supports.manual.erase(model->supports.manual.begin() + row);
    refreshManualSupports();
    applySupports();
}

void MainWindow::refreshManualSupports() {
    m_manual_supports->clear();
    const ModelState *model = selectedModel();
    if (!model)
        return;
    for (const ManualSupport &point : model->supports.manual) {
        QString label = triplet(point.contact);
        if (point.base_z)
            label += ", base Z=" + number(*point.base_z);
        m_manual_supports->addItem(label);
    }
}

bool MainWindow::rebuildModel(ModelState &model, bool show_error) {
    const QString scene_path = makeTemporaryPath(".acv");
    if (scene_path.isEmpty()) {
        if (show_error)
            QMessageBox::critical(this, "Slicer preview",
                                  "Could not allocate a temporary viewer scene.");
        return false;
    }
    const ProcessResult result =
        runRust(modelArguments(model, "viewer-scene", scene_path),
                "Preparing " + model.display_name);
    if (!result.ok) {
        QFile::remove(scene_path);
        if (show_error)
            QMessageBox::critical(this, "Slicer preview", result.output);
        return false;
    }
    SceneData visual;
    QString error;
    const bool loaded = loadScene(scene_path, visual, error);
    QFile::remove(scene_path);
    if (!loaded) {
        if (show_error)
            QMessageBox::critical(this, "Slicer preview", error);
        return false;
    }
    model.visual = std::move(visual);
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
        appendLog("Plate changed: previous PWSZ was invalidated.");
    m_last_pwsz.clear();
    m_cloud_file_id.reset();
    m_sliced_layers.clear();
    if (m_view)
        m_view->setLayers({});
    if (m_output_label)
        m_output_label->setText("No current PWSZ. Press Slice & Run to create one.");
    if (m_cloud_file_label)
        m_cloud_file_label->setText("Cloud file: not uploaded");
    updateLayerControls();
    updateActions();
}

void MainWindow::updateActions() {
    if (!m_slice_action)
        return;
    const bool has_model = std::any_of(m_models.begin(), m_models.end(),
                                       [](const auto &model) { return model->editable; });
    const bool has_artwork = !m_pcb_gerbers.isEmpty() || !m_pcb_drills.isEmpty();
    const bool selected = selectedModel() != nullptr;
    m_remove_action->setEnabled(selected);
    m_layout_action->setEnabled(has_model);
    m_slice_action->setEnabled(has_model || has_artwork);
    if (has_artwork && !has_model)
        m_slice_action->setText("Slice & Upload…");
    else
        m_slice_action->setText("Slice & Run…");
    m_pause_action->setEnabled(true);
    m_resume_action->setEnabled(true);
    m_stop_action->setEnabled(true);
    m_status_action->setEnabled(true);
}

void MainWindow::browseCloudConfig() {
    const QString path = QFileDialog::getOpenFileName(
        this, "Select Photon Workshop account config", m_cloud_config->text(),
        "Configuration (global_config.ini *.ini);;All files (*)");
    if (!path.isEmpty()) {
        m_cloud_config->setText(path);
        saveSettings();
    }
}

bool MainWindow::connectToCloud(bool show_result) {
    const QFileInfo config(m_cloud_config->text().trimmed());
    if (!config.isFile()) {
        const QString message = "Select an existing global_config.ini first.";
        appendLog("Cloud connection skipped: " + message);
        if (show_result)
            QMessageBox::warning(this, "Cloud connection", message);
        return false;
    }
    const ProcessResult status =
        runRust({"cloud-status-detail", config.absoluteFilePath()}, "Connecting to printer");
    if (!status.ok) {
        if (show_result)
            QMessageBox::critical(this, "Cloud connection", status.output);
        return false;
    }

    QSet<QString> printer_ids;
    const QRegularExpression id_pattern("id=Some\\(\\\"(\\d+)\\\"\\)");
    QRegularExpressionMatchIterator matches = id_pattern.globalMatch(status.output);
    while (matches.hasNext())
        printer_ids.insert(matches.next().captured(1));
    QString selected = m_printer_id->text().trimmed();
    if (!printer_ids.contains(selected)) {
        if (printer_ids.size() == 1)
            selected = *printer_ids.constBegin();
        else {
            const QString message =
                printer_ids.isEmpty()
                    ? "Cloud returned no printer ID."
                    : "Cloud returned multiple printers; enter the desired printer ID.";
            appendLog("Cloud connection incomplete: " + message);
            if (show_result)
                QMessageBox::warning(this, "Cloud connection", message + "\n\n" + status.output);
            return false;
        }
    }
    m_cloud_config->setText(config.absoluteFilePath());
    m_printer_id->setText(selected);
    saveSettings();
    const bool online = status.output.contains("device_status=Some(1)");
    const bool idle = status.output.contains("is_printing=Some(1)");
    appendLog(QString("Connected to Cloud printer %1: online=%2, idle=%3. Settings saved.")
                  .arg(selected, online ? "yes" : "no", idle ? "yes" : "no"));
    if (show_result) {
        QMessageBox::information(
            this, "Cloud connection",
            QString("Connected to printer %1.\nOnline: %2\nIdle: %3\n\nThe account path and "
                    "printer ID will be restored next time.")
                .arg(selected, online ? "yes" : "no", idle ? "yes" : "no"));
    }
    return true;
}

void MainWindow::loginToCloud() {
    QString config_path = m_cloud_config->text().trimmed();
    if (config_path.isEmpty()) {
        config_path = QFileDialog::getSaveFileName(
            this, "Store Anycubic account token", QDir::home().filePath("anycubic-cloud.ini"),
            "Configuration (*.ini);;All files (*)");
        if (config_path.isEmpty())
            return;
        if (QFileInfo(config_path).suffix().isEmpty())
            config_path += ".ini";
        m_cloud_config->setText(QFileInfo(config_path).absoluteFilePath());
    }

    static const QUrl authorization_url(
        "https://uc.makeronline.com/login/oauth/authorize?"
        "client_id=25ec844d72180b556acc&response_type=code&scope=read&"
        "application=ac_slicer_lcd&redirect_uri=acpws%3A%2F%2Flogin");
    if (!QDesktopServices::openUrl(authorization_url)) {
        QMessageBox::warning(
            this, "Anycubic login",
            "The browser could not be opened. Copy this address into a browser:\n\n" +
                authorization_url.toString());
    }

    bool accepted = false;
    const QString callback = QInputDialog::getText(
        this, "Complete Anycubic login",
        "Sign in only on the Anycubic web page. After it redirects, paste the complete "
        "acpws://login?... address (or just its code) here. The password is never given to "
        "this application:",
        QLineEdit::Normal, QString(), &accepted);
    if (!accepted || callback.trimmed().isEmpty())
        return;

    // Pass the short-lived OAuth code over stdin so it is absent from the GUI
    // log and the child process command line.
    const ProcessResult login = runRust(
        {"cloud-login-complete", QFileInfo(config_path).absoluteFilePath()},
        "Completing Anycubic OAuth login", 60 * 1000, callback.trimmed().toUtf8());
    if (!login.ok) {
        QMessageBox::critical(this, "Anycubic login", login.output);
        return;
    }

    m_cloud_config->setText(QFileInfo(config_path).absoluteFilePath());
    const bool connected = connectToCloud(false);
    saveSettings();
    QMessageBox::information(
        this, "Anycubic login",
        connected ? "Login completed and the account printer was connected. Settings saved."
                  : "Login completed and the token was saved, but automatic printer detection "
                    "failed. Use Connect / detect printer to retry.");
}

void MainWindow::sliceAndRun() {
    const bool has_model = std::any_of(m_models.begin(), m_models.end(),
                                       [](const auto &model) { return model->editable; });
    const bool has_artwork = !m_pcb_gerbers.isEmpty() || !m_pcb_drills.isEmpty();
    if (has_model && has_artwork) {
        QMessageBox::warning(
            this, "Mixed input types",
            "A resin 3D job and a one-layer PCB exposure cannot share one PWSZ. Remove either "
            "the 3D models or the Gerber/DRL files, then press Slice & Run again.");
        return;
    }
    if (!has_model && m_pcb_gerbers.isEmpty()) {
        QMessageBox::warning(this, "Slice & Run",
                             has_artwork ? "A DRL file requires at least one Gerber file."
                                         : "Import a 3D model or Gerber file first.");
        return;
    }
    qulonglong printer_id = 0;
    if (!validateCloudInputs(printer_id))
        return;
    const bool sliced = has_model ? sliceProject() : slicePcb();
    if (!sliced || !uploadToCloud())
        return;
    if (m_last_job_is_pcb) {
        QMessageBox::information(
            this, "PCB uploaded—not started",
            "The Gerber PWSZ was generated, parsed by Anycubic Cloud and delivered to the "
            "printer, but it was NOT started. A custom Gerber image currently requires the "
            "normal print/start path, and that path homes/moves Z even when lift is zero. "
            "Remove the build platform and verify clearance before starting this Cloud file "
            "manually. The motion-free LCD test on Printer/Cloud uses only built-in patterns.");
        appendLog("PCB Cloud file uploaded; automatic print/start suppressed to prevent Z motion.");
        return;
    }
    startPrinting();
}

bool MainWindow::sliceProject() {
    if (m_models.empty())
        return false;
    QString suggested = QDir::current().filePath("plate.pwsz");
    if (!m_models.empty())
        suggested = QFileInfo(m_models.front()->path).absoluteDir().filePath("plate.pwsz");
    QString output = QFileDialog::getSaveFileName(this, "Write print job", suggested,
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
    const ProcessResult sliced =
        runRust({"project-slice", project_path, output}, "Slicing plate", 60 * 60 * 1000);
    if (!sliced.ok) {
        QFile::remove(project_path);
        QMessageBox::critical(this, "Slice", sliced.output);
        return false;
    }
    m_last_pwsz = QFileInfo(output).absoluteFilePath();
    m_last_job_is_pcb = false;
    m_last_pcb_exposure = 0.0;
    m_cloud_file_id.reset();
    m_output_label->setText("Current PWSZ: " + m_last_pwsz);
    m_cloud_file_label->setText("Cloud file: not uploaded");

    const QString scene_path = makeTemporaryPath(".acv");
    const ProcessResult preview = runRust(
        {"project-viewer-scene", project_path, scene_path}, "Preparing sliced-layer viewer",
        60 * 60 * 1000);
    QFile::remove(project_path);
    if (preview.ok) {
        SceneData scene;
        if (loadScene(scene_path, scene, error)) {
            m_sliced_layers = std::move(scene.layers);
            m_view->setLayers(m_sliced_layers);
            updateLayerControls();
        } else {
            QMessageBox::warning(this, "Layer preview", error);
        }
    } else {
        QMessageBox::warning(this, "Layer preview",
                             "PWSZ was created, but layer preview failed:\n" + preview.output);
    }
    QFile::remove(scene_path);
    updateActions();
    appendLog("3D PWSZ ready: " + m_last_pwsz);
    return true;
}

bool MainWindow::previewPcb() {
    if (m_pcb_gerbers.isEmpty()) {
        QMessageBox::warning(this, "PCB preview", "Import at least one Gerber file first.");
        return false;
    }
    const QString scene_path = makeTemporaryPath(".acv");
    if (scene_path.isEmpty()) {
        QMessageBox::critical(this, "PCB preview", "Could not allocate a temporary scene.");
        return false;
    }
    const ProcessResult result =
        runRust(pcbArguments("pcb-viewer-scene", scene_path), "Compositing PCB LCD layer");
    if (!result.ok) {
        QFile::remove(scene_path);
        QMessageBox::critical(this, "PCB preview", result.output);
        return false;
    }
    SceneData scene;
    QString error;
    if (!loadScene(scene_path, scene, error)) {
        QFile::remove(scene_path);
        QMessageBox::critical(this, "PCB preview", error);
        return false;
    }
    QFile::remove(scene_path);
    m_sliced_layers = std::move(scene.layers);
    m_view->setBuildVolume(scene.build_volume);
    m_view->setLayers(m_sliced_layers);
    m_view_mode->setCurrentIndex(1);
    m_layer_slider->setValue(0);
    updateLayerControls();
    m_pcb_status->setText(
        QString("Preview ready: one LCD layer, %1 s exposure. Cyan pixels are exposed; "
                "dark pixels remain masked.")
            .arg(number(m_pcb_exposure->value())));
    return true;
}

bool MainWindow::slicePcb() {
    if (m_pcb_gerbers.isEmpty()) {
        QMessageBox::warning(this, "PCB slice", "Import at least one Gerber file first.");
        return false;
    }
    const QString suggested =
        QFileInfo(m_pcb_gerbers.front()).absoluteDir().filePath("pcb-exposure.pwsz");
    QString output = QFileDialog::getSaveFileName(this, "Write one-layer PCB job", suggested,
                                                  "Anycubic print job (*.pwsz)");
    if (output.isEmpty())
        return false;
    if (QFileInfo(output).suffix().isEmpty())
        output += ".pwsz";
    const ProcessResult result = runRust(pcbArguments("pcb-slice", output),
                                         "Creating one-layer PCB exposure job",
                                         60 * 60 * 1000);
    if (!result.ok) {
        QMessageBox::critical(this, "PCB slice", result.output);
        return false;
    }
    m_last_pwsz = QFileInfo(output).absoluteFilePath();
    m_last_job_is_pcb = true;
    m_last_pcb_exposure = m_pcb_exposure->value();
    m_cloud_file_id.reset();
    m_output_label->setText("Current PCB PWSZ: " + m_last_pwsz);
    m_cloud_file_label->setText("Cloud file: not uploaded");
    m_pcb_status->setText(
        QString("PCB job ready: %1 s, one layer. Upload does not start exposure.")
            .arg(number(m_last_pcb_exposure)));
    updateActions();
    if (!previewPcb())
        return false;
    appendLog("PCB PWSZ ready: " + m_last_pwsz);
    return true;
}

bool MainWindow::uploadToCloud() {
    if (m_last_pwsz.isEmpty())
        return false;
    qulonglong printer_id = 0;
    if (!validateCloudInputs(printer_id))
        return false;
    const ProcessResult result =
        runRust({"cloud-upload", m_cloud_config->text(), QString::number(printer_id),
                 m_last_pwsz},
                "Uploading PWSZ to Anycubic Cloud", 60 * 60 * 1000);
    if (!result.ok) {
        QMessageBox::critical(this, "Cloud upload", result.output);
        return false;
    }
    const QRegularExpression expression("Cloud file\\s+(\\d+)");
    const QRegularExpressionMatch match = expression.match(result.output);
    if (!match.hasMatch()) {
        QMessageBox::critical(this, "Cloud upload",
                              "Upload succeeded but the Rust response did not contain a cloud "
                              "file ID. Printing was not started.\n\n" + result.output);
        return false;
    }
    m_cloud_file_id = match.captured(1).toULongLong();
    m_cloud_file_label->setText("Cloud file ID: " + QString::number(*m_cloud_file_id));
    updateActions();
    appendLog("Cloud upload completed; waiting for final start confirmation.");
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
        warning =
            "This will start a REAL one-layer UV exposure on printer " +
            QString::number(printer_id) + " for " + number(m_last_pcb_exposure) +
            " seconds using cloud file " + QString::number(*m_cloud_file_id) +
            ". The Rust API disables automatic resin feed, preheating, stirring, auto-top, "
            "intelligent release, off-light compensation and failure detection for this job. "
            "However, this is still the normal print-start path: firmware WILL perform its own "
            "homing/non-editable checks. Confirm that the BUILD PLATFORM HAS BEEN REMOVED, then "
            "verify PCB/photoresist placement, polarity, mirror direction, cover and eye/skin "
            "UV protection.";
    } else {
        warning = "This will start a real resin print on printer " +
                  QString::number(printer_id) + " using cloud file " +
                  QString::number(*m_cloud_file_id) +
                  ". Verify the vat, resin, platform and model before continuing.";
    }
    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, "Start real printer", warning, QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;
    const QString operation = m_last_job_is_pcb ? "cloud-start-pcb" : "cloud-start";
    const ProcessResult result = runRust(
        {operation, m_cloud_config->text(), QString::number(printer_id),
         QString::number(*m_cloud_file_id)},
        "Starting print");
    if (!result.ok)
        QMessageBox::critical(this, "Run printing", result.output);
    else
        QMessageBox::information(this, "Run printing", result.output);
}

void MainWindow::pausePrinting() {
    qulonglong printer_id = 0;
    if (!validateCloudInputs(printer_id))
        return;
    const ProcessResult result = runRust(
        {"cloud-pause", m_cloud_config->text(), QString::number(printer_id)}, "Pausing print");
    if (!result.ok)
        QMessageBox::critical(this, "Pause", result.output);
    else
        QMessageBox::information(this, "Pause", result.output);
}

void MainWindow::resumePrinting() {
    qulonglong printer_id = 0;
    if (!validateCloudInputs(printer_id))
        return;
    const ProcessResult result = runRust(
        {"cloud-resume", m_cloud_config->text(), QString::number(printer_id)}, "Resuming print");
    if (!result.ok)
        QMessageBox::critical(this, "Resume", result.output);
    else
        QMessageBox::information(this, "Resume", result.output);
}

void MainWindow::stopPrinting() {
    qulonglong printer_id = 0;
    if (!validateCloudInputs(printer_id))
        return;
    const QMessageBox::StandardButton answer = QMessageBox::critical(
        this, "Stop real printer",
        "Stop the active print on printer " + QString::number(printer_id) +
            "? The Rust API will resolve the active project automatically.",
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;
    const ProcessResult result = runRust(
        {"cloud-stop", m_cloud_config->text(), QString::number(printer_id)}, "Stopping print");
    if (!result.ok)
        QMessageBox::critical(this, "Stop", result.output);
    else
        QMessageBox::information(this, "Stop", result.output);
}

void MainWindow::startBuiltinExposure() {
    qulonglong printer_id = 0;
    if (!validateCloudInputs(printer_id))
        return;
    const int pattern = m_exposure_pattern->currentData().toInt();
    const int seconds = m_exposure_duration->value();
    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, "Run UV LCD test",
        "This will turn on the printer UV matrix for " + QString::number(seconds) +
            " seconds using built-in raw pattern " + QString::number(pattern) +
            ". The command has no print/Z movement fields, but it does NOT display your "
            "Gerber. Close the cover and protect eyes and skin from UV exposure.",
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;
    const ProcessResult result = runRust(
        {"cloud-exposure-start", m_cloud_config->text(), QString::number(printer_id),
         QString::number(pattern), QString::number(seconds)},
        "Starting built-in LCD exposure");
    if (!result.ok)
        QMessageBox::critical(this, "LCD exposure", result.output);
    else
        QMessageBox::information(this, "LCD exposure", result.output);
}

void MainWindow::stopBuiltinExposure() {
    qulonglong printer_id = 0;
    if (!validateCloudInputs(printer_id))
        return;
    const ProcessResult result =
        runRust({"cloud-exposure-stop", m_cloud_config->text(), QString::number(printer_id)},
                "Stopping built-in LCD exposure");
    if (!result.ok)
        QMessageBox::critical(this, "Stop LCD exposure", result.output);
    else
        QMessageBox::information(this, "Stop LCD exposure", result.output);
}

void MainWindow::requestPrinterStatus() {
    const QFileInfo config(m_cloud_config->text());
    if (!config.isFile()) {
        QMessageBox::warning(this, "Printer status", "Select an existing global_config.ini first.");
        return;
    }
    const ProcessResult result =
        runRust({"cloud-status-detail", config.absoluteFilePath()}, "Reading printer status");
    if (!result.ok)
        QMessageBox::critical(this, "Printer status", result.output);
    else
        QMessageBox::information(this, "Printer status", result.output);
}

ProcessResult MainWindow::runRust(const QStringList &arguments, const QString &title,
                                  int timeout_ms, const QByteArray &standard_input) {
    const QString program = rustCli();
    if (program.isEmpty()) {
        return {false,
                "Rust executable was not found. Build it with cargo build --release or set "
                "ANYCUBICSTUDIO_CLI."};
    }
    QStringList command;
    command << quoted(program);
    for (const QString &argument : arguments)
        command << quoted(argument);
    appendLog("$ " + command.join(' '));

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    if (!process.waitForStarted(5000)) {
        const QString error = "Could not start Rust API: " + process.errorString();
        appendLog(error);
        return {false, error};
    }
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
        if (progress.wasCanceled()) {
            process.kill();
            process.waitForFinished(3000);
            const QString output = QString::fromUtf8(process.readAll());
            appendLog(output + "\nOperation cancelled.");
            return {false, "Operation cancelled."};
        }
        if (timer.elapsed() > timeout_ms) {
            process.kill();
            process.waitForFinished(3000);
            const QString output = QString::fromUtf8(process.readAll());
            appendLog(output + "\nOperation timed out.");
            return {false, "Rust API timed out after " + QString::number(timeout_ms) + " ms."};
        }
    }
    const QString output = QString::fromUtf8(process.readAll()).trimmed();
    if (!output.isEmpty())
        appendLog(output);
    const bool ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    return {ok, output.isEmpty() ? (ok ? "Done." : "Rust API failed without output.") : output};
}

QString MainWindow::rustCli() const {
    const QString environment = qEnvironmentVariable("ANYCUBICSTUDIO_CLI");
    if (!environment.isEmpty() && QFileInfo::exists(environment))
        return QFileInfo(environment).absoluteFilePath();
    const QString sibling = QDir(QCoreApplication::applicationDirPath()).filePath("anycubicstudio");
    if (QFileInfo::exists(sibling))
        return sibling;
    const QString local = QDir::current().filePath("target/release/anycubicstudio");
    if (QFileInfo::exists(local))
        return QFileInfo(local).absoluteFilePath();
    const QString debug = QDir::current().filePath("target/debug/anycubicstudio");
    if (QFileInfo::exists(debug))
        return QFileInfo(debug).absoluteFilePath();
    return {};
}

QStringList MainWindow::modelArguments(const ModelState &model, const QString &operation,
                                       const QString &output) const {
    QVector3D scale = model.scale;
    if (model.mirror_x)
        scale.setX(-scale.x());
    if (model.mirror_y)
        scale.setY(-scale.y());
    if (model.mirror_z)
        scale.setZ(-scale.z());
    QStringList result{operation,
                       model.path,
                       output,
                       "--build-volume",
                       number(m_zone[0]->value()) + "," + number(m_zone[1]->value()) + "," +
                           number(m_zone[2]->value()),
                       "--move",
                       triplet(model.move),
                       "--rotate",
                       triplet(model.rotate),
                       "--scale",
                       triplet(scale),
                       "--supports",
                       model.supports.enabled ? "medium" : "none",
                       "--support-auto",
                       model.supports.automatic ? "on" : "off",
                       "--support-shape",
                       model.supports.shape,
                       "--support-angle",
                       number(model.supports.angle),
                       "--support-spacing",
                       number(model.supports.spacing),
                       "--support-tip",
                       number(model.supports.tip),
                       "--support-trunk",
                       number(model.supports.trunk),
                       "--support-base",
                       number(model.supports.base),
                       "--support-tip-height",
                       number(model.supports.tip_height),
                       "--support-base-height",
                       number(model.supports.base_height),
                       "--support-penetration",
                       number(model.supports.penetration),
                       "--support-min-height",
                       number(model.supports.minimum_height),
                       "--support-max-height",
                       number(model.supports.maximum_height),
                       "--support-segments",
                       QString::number(model.supports.segments)};
    if (model.supports.enabled) {
        for (const ManualSupport &support : model.supports.manual) {
            QString value = triplet(support.contact);
            if (support.base_z)
                value += "," + number(*support.base_z);
            result << "--manual-support" << value;
        }
    }
    return result;
}

QStringList MainWindow::pcbArguments(const QString &operation, const QString &output) const {
    if (m_pcb_gerbers.isEmpty())
        return {};
    QStringList result{operation,
                       m_pcb_gerbers.front(),
                       output,
                       "--exposure",
                       number(m_pcb_exposure->value()),
                       "--drill-mode",
                       m_pcb_drill_mode->currentText(),
                       "--rotate",
                       number(m_pcb_rotate->value()),
                       "--scale",
                       number(m_pcb_scale->value()),
                       "--move",
                       number(m_pcb_move[0]->value()) + "," +
                           number(m_pcb_move[1]->value()),
                       "--invert-margin",
                       number(m_pcb_invert_margin->value()),
                       "--simplify",
                       number(m_pcb_simplify->value()),
                       "--layer-height",
                       number(m_layer_height->value()),
                       "--lift-height",
                       number(m_pcb_lift_height->value()),
                       "--lift-speed",
                       number(m_lift_speed->value()),
                       "--build-volume",
                       number(m_zone[0]->value()) + "," + number(m_zone[1]->value()) + "," +
                           number(m_zone[2]->value())};
    for (int index = 1; index < m_pcb_gerbers.size(); ++index)
        result << "--gerber" << m_pcb_gerbers[index];
    for (const QString &drill : m_pcb_drills)
        result << "--drill" << drill;
    // The GUI already displays and persists the effective choice. Disable the
    // CLI's filename default so manually clearing the checkbox is respected.
    result << "--no-auto-mirror-front";
    if (m_pcb_invert->isChecked())
        result << "--invert";
    if (m_pcb_mirror_x->isChecked())
        result << "--mirror-layers";
    if (m_pcb_mirror_y->isChecked())
        result << "--mirror-y";
    return result;
}

QByteArray MainWindow::projectJson() const {
    const QJsonObject default_transform{{"translation_mm", QJsonArray{0.0, 0.0, 0.0}},
                                        {"rotation_deg", QJsonArray{0.0, 0.0, 0.0}},
                                        {"scale", QJsonArray{1.0, 1.0, 1.0}},
                                        {"auto_center_xy", true},
                                        {"place_on_bed", true}};
    SupportSettings default_support;
    QJsonObject disabled_support = supportJson(default_support);
    disabled_support.insert("enabled", false);
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
                             {"supports", disabled_support},
                             {"xy_offset_mm", 0.0},
                             {"hollow", false},
                             {"gmsh_path", "gmsh"}};
    QJsonArray models;
    for (const auto &model : m_models) {
        if (!model->editable)
            continue;
        models.append(QJsonObject{{"input", model->path},
                                  {"transform", transformJson(*model)},
                                  {"supports", supportJson(model->supports)}});
    }
    return QJsonDocument(QJsonObject{{"config", config}, {"models", models}})
        .toJson(QJsonDocument::Indented);
}

QString MainWindow::makeTemporaryPath(const QString &suffix) const {
    const QString pattern = QDir::temp().filePath("anycubic-gui-XXXXXX" + suffix);
    QTemporaryFile file(pattern);
    file.setAutoRemove(false);
    if (!file.open())
        return {};
    const QString path = file.fileName();
    file.close();
    QFile::remove(path);
    return path;
}

bool MainWindow::writeTemporaryProject(QString &path, QString &error) const {
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
    if (file.write(projectJson()) < 0) {
        error = file.errorString();
        file.remove();
        return false;
    }
    return true;
}

bool MainWindow::validateCloudInputs(qulonglong &printer_id) {
    const QFileInfo config(m_cloud_config->text());
    if (!config.isFile()) {
        QMessageBox::warning(this, "Cloud",
                             "Select an existing Photon Workshop global_config.ini.");
        return false;
    }
    bool ok = false;
    printer_id = m_printer_id->text().trimmed().toULongLong(&ok);
    if (!ok || printer_id == 0) {
        QMessageBox::warning(this, "Cloud",
                             "Enter the numeric printer ID shown by cloud-status.");
        return false;
    }
    return true;
}

void MainWindow::appendLog(const QString &text) {
    if (!m_log || text.trimmed().isEmpty())
        return;
    m_log->appendPlainText(text.trimmed());
}
