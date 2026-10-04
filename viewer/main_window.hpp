#pragma once

#include "scene.hpp"

#include <QMainWindow>
#include <QVector3D>

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
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class PrintView;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(const QStringList &initial_files = {}, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void buildInterface();
    QWidget *buildCentre();
    QWidget *buildTransformPanel();
    QWidget *buildSupportPanel();
    QWidget *buildPrintPanel();
    QWidget *buildPcbPanel();
    QWidget *buildCloudPanel();
    void connectActions();
    void loadSettings();
    void saveSettings() const;

    void importDialog();
    void importPaths(const QStringList &paths);
    void importPcbPaths(const QStringList &paths);
    void updatePcbAutoMirror();
    void removeSelectedPcbFile();
    void refreshPcbFiles();
    bool previewPcb();
    bool slicePcb();
    bool importPath(const QString &path);
    void removeSelected();
    void selectModel(int row);
    ModelState *selectedModel();
    const ModelState *selectedModel() const;
    void refreshModelList();
    void refreshView();
    void loadModelControls();
    void applyTransform();
    void resetTransform();
    void centreSelected();
    void placeSelectedOnBed();
    void autoLayout();
    void applySupports();
    void applySupportPreset(int index);
    void addManualSupport();
    void removeManualSupport();
    void refreshManualSupports();
    bool rebuildModel(ModelState &model, bool show_error = true);

    void updateLayerControls();
    void updateLayerLabel(int layer);
    void printingZoneChanged();
    void invalidateSlice();
    void updateActions();
    void browseCloudConfig();
    void loginToCloud();
    bool connectToCloud(bool show_result);
    bool sliceProject();
    void sliceAndRun();
    bool uploadToCloud();
    void startPrinting();
    void pausePrinting();
    void resumePrinting();
    void stopPrinting();
    void startBuiltinExposure();
    void stopBuiltinExposure();
    void requestPrinterStatus();

    ProcessResult runRust(const QStringList &arguments, const QString &title,
                          int timeout_ms = 15 * 60 * 1000,
                          const QByteArray &standard_input = {});
    QString rustCli() const;
    QStringList modelArguments(const ModelState &model, const QString &operation,
                               const QString &output) const;
    QStringList pcbArguments(const QString &operation, const QString &output) const;
    QByteArray projectJson() const;
    QString makeTemporaryPath(const QString &suffix) const;
    bool writeTemporaryProject(QString &path, QString &error) const;
    bool validateCloudInputs(qulonglong &printer_id);
    void appendLog(const QString &text);

    PrintView *m_view = nullptr;
    QListWidget *m_model_list = nullptr;
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

    QListWidget *m_pcb_files = nullptr;
    QDoubleSpinBox *m_pcb_exposure = nullptr;
    QComboBox *m_pcb_drill_mode = nullptr;
    QCheckBox *m_pcb_invert = nullptr;
    QCheckBox *m_pcb_auto_mirror = nullptr;
    QCheckBox *m_pcb_mirror_x = nullptr;
    QCheckBox *m_pcb_mirror_y = nullptr;
    QDoubleSpinBox *m_pcb_rotate = nullptr;
    QDoubleSpinBox *m_pcb_scale = nullptr;
    QDoubleSpinBox *m_pcb_move[2]{};
    QDoubleSpinBox *m_pcb_invert_margin = nullptr;
    QDoubleSpinBox *m_pcb_simplify = nullptr;
    QDoubleSpinBox *m_pcb_lift_height = nullptr;
    QLabel *m_pcb_status = nullptr;

    QAction *m_remove_action = nullptr;
    QAction *m_layout_action = nullptr;
    QAction *m_slice_action = nullptr;
    QAction *m_pause_action = nullptr;
    QAction *m_resume_action = nullptr;
    QAction *m_stop_action = nullptr;
    QAction *m_status_action = nullptr;

    std::vector<std::unique_ptr<ModelState>> m_models;
    SupportSettings m_default_supports;
    QStringList m_pcb_gerbers;
    QStringList m_pcb_drills;
    QVector<LayerData> m_sliced_layers;
    QString m_last_pwsz;
    std::optional<qulonglong> m_cloud_file_id;
    bool m_last_job_is_pcb = false;
    double m_last_pcb_exposure = 0.0;
    bool m_loading_controls = false;
};
