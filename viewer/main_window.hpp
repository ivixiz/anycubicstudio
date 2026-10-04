#pragma once

#include "scene.hpp"

#include <QMainWindow>

#include <memory>
#include <optional>
#include <vector>

class QAction;
class QByteArray;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QDragEnterEvent;
class QDropEvent;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSlider;
class QSpinBox;
class QTabWidget;
class QTimer;
class PrintView;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(const QStringList &initial_files = {}, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void buildInterface();
    QWidget *buildCentre();
    QWidget *buildTransformPanel();
    QWidget *buildSupportPanel();
    QWidget *buildPcbPanel();
    QWidget *buildPrintPanel();
    QWidget *buildCloudPanel();
    void connectActions();
    void loadSettings();
    void saveSettings() const;

    void importDialog();
    void importPaths(const QStringList &paths);
    bool importPath(const QString &path);
    void deleteSelection();
    void autoLayout();
    void selectionFromList();
    void selectionFromView(int index, Qt::KeyboardModifiers modifiers);
    std::vector<SceneObject *> selectedObjects();
    std::vector<const SceneObject *> selectedObjects() const;
    SceneObject *firstSelectedModel();
    SceneObject *firstSelectedArtwork();
    void refreshObjectList();
    void refreshView();
    void refreshPropertyPanels();
    void loadTransformControls();
    void loadSupportControls();
    void loadPcbControls();
    void applyTransformLive();
    void applySupportsLive();
    void applyPcbLive();
    void resetTransform();
    void centreSelection();
    void placeSelectionOnBed();
    void applySupportPreset(int index);
    void addManualSupport();
    void removeManualSupport();
    void refreshManualSupports();
    void gizmoTransform(int mode, const QVector3D &delta);
    void transformVisual(SceneObject &object, int mode, const QVector3D &delta);
    void scheduleRebuild();
    void rebuildSelectedPreviews();
    bool rebuildObject(SceneObject &object, bool show_error = true);

    void updateLayerControls();
    void updateLayerLabel(int layer);
    void printingZoneChanged();
    void invalidateSlice();
    void updateActions();
    bool sliceScene();
    void runPrepared();
    bool uploadToCloud();
    void startPrinting();

    void browseCloudConfig();
    void loginToCloud();
    bool connectToCloud(bool show_result);
    bool validateCloudInputs(qulonglong &printer_id);
    void pausePrinting();
    void resumePrinting();
    void stopPrinting();
    void requestPrinterStatus();
    void startBuiltinExposure();
    void stopBuiltinExposure();

    ProcessResult runRust(const QStringList &arguments, const QString &title,
                          int timeout_ms = 15 * 60 * 1000,
                          const QByteArray &standard_input = {});
    QString rustCli() const;
    QByteArray projectJson(const SceneObject *isolated = nullptr,
                           bool expose_isolated_drill = false) const;
    QString makeTemporaryPath(const QString &suffix) const;
    bool writeTemporaryProject(QString &path, QString &error,
                               const SceneObject *isolated = nullptr,
                               bool expose_isolated_drill = false) const;
    void appendLog(const QString &text);

    PrintView *m_view = nullptr;
    QListWidget *m_object_list = nullptr;
    QTabWidget *m_tabs = nullptr;
    QWidget *m_transform_panel = nullptr;
    QWidget *m_support_panel = nullptr;
    QWidget *m_pcb_panel = nullptr;
    QComboBox *m_view_mode = nullptr;
    QCheckBox *m_show_model = nullptr;
    QCheckBox *m_show_supports = nullptr;
    QSlider *m_layer_slider = nullptr;
    QLabel *m_layer_label = nullptr;
    QPlainTextEdit *m_log = nullptr;

    QDoubleSpinBox *m_move[3]{};
    QDoubleSpinBox *m_rotate[3]{};
    QDoubleSpinBox *m_scale[3]{};
    QCheckBox *m_mirror[3]{};

    QComboBox *m_support_preset = nullptr;
    QCheckBox *m_support_enabled = nullptr;
    QCheckBox *m_support_auto = nullptr;
    QComboBox *m_support_shape = nullptr;
    QDoubleSpinBox *m_support_angle = nullptr;
    QDoubleSpinBox *m_support_spacing = nullptr;
    QDoubleSpinBox *m_support_tip = nullptr;
    QDoubleSpinBox *m_support_trunk = nullptr;
    QDoubleSpinBox *m_support_base = nullptr;
    QDoubleSpinBox *m_support_tip_height = nullptr;
    QDoubleSpinBox *m_support_base_height = nullptr;
    QDoubleSpinBox *m_support_penetration = nullptr;
    QDoubleSpinBox *m_support_min_height = nullptr;
    QDoubleSpinBox *m_support_max_height = nullptr;
    QSpinBox *m_support_segments = nullptr;
    QListWidget *m_manual_supports = nullptr;

    QDoubleSpinBox *m_pcb_exposure = nullptr;
    QComboBox *m_pcb_drill_mode = nullptr;
    QCheckBox *m_pcb_invert = nullptr;
    QDoubleSpinBox *m_pcb_invert_margin = nullptr;
    QDoubleSpinBox *m_pcb_simplify = nullptr;
    QLabel *m_pcb_status = nullptr;

    QDoubleSpinBox *m_zone[3]{};
    QDoubleSpinBox *m_layer_height = nullptr;
    QSpinBox *m_bottom_layers = nullptr;
    QDoubleSpinBox *m_bottom_exposure = nullptr;
    QDoubleSpinBox *m_exposure = nullptr;
    QDoubleSpinBox *m_lift_height = nullptr;
    QDoubleSpinBox *m_lift_speed = nullptr;
    QDoubleSpinBox *m_off_time = nullptr;
    QLabel *m_output_label = nullptr;

    QLineEdit *m_cloud_config = nullptr;
    QLineEdit *m_printer_id = nullptr;
    QLabel *m_cloud_file_label = nullptr;
    QComboBox *m_exposure_pattern = nullptr;
    QSpinBox *m_exposure_duration = nullptr;

    QAction *m_layout_action = nullptr;
    QAction *m_slice_action = nullptr;
    QAction *m_run_action = nullptr;
    QAction *m_pause_action = nullptr;
    QAction *m_resume_action = nullptr;
    QAction *m_stop_action = nullptr;
    QAction *m_status_action = nullptr;
    QTimer *m_rebuild_timer = nullptr;

    std::vector<std::unique_ptr<SceneObject>> m_objects;
    SupportSettings m_default_supports;
    PcbSettings m_default_pcb;
    QVector<LayerData> m_sliced_layers;
    QString m_last_pwsz;
    std::optional<qulonglong> m_cloud_file_id;
    bool m_last_job_is_pcb = false;
    double m_last_pcb_exposure = 0.0;
    bool m_loading_controls = false;
    quint64 m_next_object_id = 1;
};
