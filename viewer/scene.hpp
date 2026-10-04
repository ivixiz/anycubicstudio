#pragma once

#include <QString>
#include <QVector>
#include <QVector2D>
#include <QVector3D>

#include <memory>
#include <optional>
#include <vector>

constexpr double M7_BUILD_X = 223.64199829101563;
constexpr double M7_BUILD_Y = 126.4800033569336;
constexpr double M7_BUILD_Z = 230.0;

struct LayerData {
    float z = 0.0f;
    QVector<QVector<QVector2D>> contours;
};

struct SceneData {
    QVector<float> model_vertices;
    QVector<float> support_vertices;
    QVector<LayerData> layers;
    QVector3D build_volume{M7_BUILD_X, M7_BUILD_Y, M7_BUILD_Z};
    QVector3D minimum{-1.0f, -1.0f, 0.0f};
    QVector3D maximum{1.0f, 1.0f, 1.0f};
};

struct ManualSupport {
    QVector3D contact;
    std::optional<double> base_z;
};

struct SupportSettings {
    bool enabled = false;
    bool automatic = true;
    QString shape = "tapered";
    double angle = 45.0;
    double spacing = 4.0;
    double tip = 0.5;
    double trunk = 1.2;
    double base = 4.5;
    double tip_height = 1.5;
    double base_height = 0.8;
    double penetration = 0.15;
    double minimum_height = 2.0;
    double maximum_height = 180.0;
    int segments = 12;
    std::vector<ManualSupport> manual;
};

enum class SceneObjectType { Model, Gerber, Drill, ViewerScene };

struct PcbSettings {
    double exposure = 60.0;
    QString drill_mode = "clear";
    bool invert = false;
    bool auto_mirror = true;
    double invert_margin = 0.0;
    double simplify = 0.012;
};

struct SceneObject {
    quint64 id = 0;
    SceneObjectType type = SceneObjectType::Model;
    QString path;
    QString display_name;
    bool visible = true;
    bool selected = false;
    QVector3D move{0.0f, 0.0f, 0.0f};
    QVector3D rotate{0.0f, 0.0f, 0.0f};
    QVector3D scale{1.0f, 1.0f, 1.0f};
    bool mirror_x = false;
    bool mirror_y = false;
    bool mirror_z = false;
    bool editable = true;
    SupportSettings supports;
    PcbSettings pcb;
    SceneData visual;

    bool isModel() const { return type == SceneObjectType::Model; }
    bool isArtwork() const {
        return type == SceneObjectType::Gerber || type == SceneObjectType::Drill;
    }
};

struct ProcessResult {
    bool ok = false;
    QString output;
};

bool supportedModel(const QString &path);
bool supportedGerber(const QString &path);
bool supportedDrill(const QString &path);
bool supportedArtwork(const QString &path);
bool loadScene(const QString &path, SceneData &scene, QString &error);
QString number(double value);
QString triplet(const QVector3D &value);
