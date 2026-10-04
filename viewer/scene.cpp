#include "scene.hpp"

#include <QDataStream>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <limits>

bool supportedModel(const QString &path) {
    const QString extension = QFileInfo(path).suffix().toLower();
    return extension == "stl" || extension == "3mf" || extension == "step" ||
           extension == "stp" || extension == "iges" || extension == "igs";
}

bool supportedGerber(const QString &path) {
    const QString extension = QFileInfo(path).suffix().toLower();
    static const QStringList extensions{
        "gbr", "ger", "pho", "gtl", "gbl", "gts", "gbs", "gto",
        "gbo", "gm1", "gm2", "gko", "gml", "gp1", "gp2"};
    return extensions.contains(extension);
}

bool supportedDrill(const QString &path) {
    const QString extension = QFileInfo(path).suffix().toLower();
    return extension == "drl" || extension == "xln" || extension == "exc";
}

bool supportedArtwork(const QString &path) {
    return supportedGerber(path) || supportedDrill(path);
}

static bool readVec3(QDataStream &stream, QVector3D &result) {
    float x, y, z;
    stream >> x >> y >> z;
    result = QVector3D(x, y, z);
    return stream.status() == QDataStream::Ok;
}

static void appendTriangle(QVector<float> &target, const QVector3D (&points)[3]) {
    QVector3D normal = QVector3D::normal(points[1] - points[0], points[2] - points[0]);
    if (normal.lengthSquared() == 0.0f)
        normal = QVector3D(0.0f, 0.0f, 1.0f);
    for (const QVector3D &point : points) {
        target << point.x() << point.y() << point.z();
        target << normal.x() << normal.y() << normal.z();
    }
}

bool loadScene(const QString &path, SceneData &scene, QString &error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return false;
    }
    if (file.read(8) != QByteArray("ACVSCN1\0", 8)) {
        error = "Not an Anycubic viewer scene";
        return false;
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    quint32 version, model_count, support_count, layer_count;
    float build_x, build_y, build_z;
    stream >> version >> build_x >> build_y >> build_z;
    stream >> model_count >> support_count >> layer_count;
    if (version != 1 || model_count > 20'000'000 || support_count > 20'000'000 ||
        layer_count > 100'000) {
        error = "Unsupported or unreasonable viewer scene header";
        return false;
    }
    scene = SceneData{};
    scene.build_volume = QVector3D(build_x, build_y, build_z);
    scene.model_vertices.reserve(static_cast<qsizetype>(model_count) * 18);
    scene.support_vertices.reserve(static_cast<qsizetype>(support_count) * 18);
    QVector3D minimum(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max());
    QVector3D maximum = -minimum;
    bool has_point = false;
    auto readTriangles = [&](quint32 count, QVector<float> &vertices) {
        for (quint32 index = 0; index < count; ++index) {
            QVector3D points[3];
            for (QVector3D &point : points) {
                if (!readVec3(stream, point))
                    return false;
                has_point = true;
                minimum.setX(std::min(minimum.x(), point.x()));
                minimum.setY(std::min(minimum.y(), point.y()));
                minimum.setZ(std::min(minimum.z(), point.z()));
                maximum.setX(std::max(maximum.x(), point.x()));
                maximum.setY(std::max(maximum.y(), point.y()));
                maximum.setZ(std::max(maximum.z(), point.z()));
            }
            appendTriangle(vertices, points);
        }
        return true;
    };
    if (!readTriangles(model_count, scene.model_vertices) ||
        !readTriangles(support_count, scene.support_vertices)) {
        error = "Viewer scene ended inside a triangle array";
        return false;
    }
    if (has_point) {
        scene.minimum = minimum;
        scene.maximum = maximum;
    }

    scene.layers.reserve(layer_count);
    quint64 total_points = 0;
    for (quint32 layer_index = 0; layer_index < layer_count; ++layer_index) {
        LayerData layer;
        quint32 contour_count;
        stream >> layer.z >> contour_count;
        if (contour_count > 1'000'000) {
            error = "Unreasonable contour count in viewer scene";
            return false;
        }
        layer.contours.reserve(contour_count);
        for (quint32 contour_index = 0; contour_index < contour_count; ++contour_index) {
            quint32 point_count;
            stream >> point_count;
            total_points += point_count;
            if (point_count > 10'000'000 || total_points > 100'000'000) {
                error = "Unreasonable point count in viewer scene";
                return false;
            }
            QVector<QVector2D> contour;
            contour.reserve(point_count);
            for (quint32 point_index = 0; point_index < point_count; ++point_index) {
                float x, y;
                stream >> x >> y;
                contour.append(QVector2D(x, y));
                has_point = true;
                minimum.setX(std::min(minimum.x(), x));
                minimum.setY(std::min(minimum.y(), y));
                minimum.setZ(std::min(minimum.z(), layer.z));
                maximum.setX(std::max(maximum.x(), x));
                maximum.setY(std::max(maximum.y(), y));
                maximum.setZ(std::max(maximum.z(), layer.z + 0.05f));
            }
            layer.contours.append(std::move(contour));
        }
        scene.layers.append(std::move(layer));
    }
    if (stream.status() != QDataStream::Ok) {
        error = "Viewer scene is truncated";
        return false;
    }
    if (has_point) {
        scene.minimum = minimum;
        scene.maximum = maximum;
    }
    return true;
}

QString number(double value) { return QString::number(value, 'g', 15); }

QString triplet(const QVector3D &value) {
    return number(value.x()) + "," + number(value.y()) + "," + number(value.z());
}
