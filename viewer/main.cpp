#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMatrix4x4>
#include <QMessageBox>
#include <QMouseEvent>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSlider>
#include <QSurfaceFormat>
#include <QVBoxLayout>
#include <QVector2D>
#include <QVector3D>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

struct LayerData {
    float z = 0.0f;
    QVector<QVector<QVector2D>> contours;
};

struct SceneData {
    QVector<float> model_vertices;
    QVector<float> support_vertices;
    QVector<LayerData> layers;
    QVector3D build_volume;
    QVector3D minimum;
    QVector3D maximum;
};

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

static bool loadScene(const QString &path, SceneData &scene, QString &error) {
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
    scene.build_volume = QVector3D(build_x, build_y, build_z);
    scene.model_vertices.reserve(static_cast<qsizetype>(model_count) * 18);
    scene.support_vertices.reserve(static_cast<qsizetype>(support_count) * 18);
    scene.minimum = QVector3D(
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    scene.maximum = -scene.minimum;
    auto readTriangles = [&](quint32 count, QVector<float> &vertices) {
        for (quint32 index = 0; index < count; ++index) {
            QVector3D points[3];
            for (QVector3D &point : points) {
                if (!readVec3(stream, point))
                    return false;
                scene.minimum.setX(std::min(scene.minimum.x(), point.x()));
                scene.minimum.setY(std::min(scene.minimum.y(), point.y()));
                scene.minimum.setZ(std::min(scene.minimum.z(), point.z()));
                scene.maximum.setX(std::max(scene.maximum.x(), point.x()));
                scene.maximum.setY(std::max(scene.maximum.y(), point.y()));
                scene.maximum.setZ(std::max(scene.maximum.z(), point.z()));
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
            }
            layer.contours.append(std::move(contour));
        }
        scene.layers.append(std::move(layer));
    }
    if (stream.status() != QDataStream::Ok) {
        error = "Viewer scene is truncated";
        return false;
    }
    return true;
}

class PrintView final : public QOpenGLWidget, protected QOpenGLFunctions {
public:
    explicit PrintView(SceneData scene, QWidget *parent = nullptr)
        : QOpenGLWidget(parent), m_scene(std::move(scene)) {
        setMinimumSize(720, 600);
        setFocusPolicy(Qt::StrongFocus);
        resetCamera();
    }

    ~PrintView() override {
        makeCurrent();
        m_model_vbo.destroy();
        m_support_vbo.destroy();
        m_vao.destroy();
        doneCurrent();
    }

    int layerCount() const { return m_scene.layers.size(); }
    float layerZ(int index) const {
        return index >= 0 && index < m_scene.layers.size() ? m_scene.layers[index].z : 0.0f;
    }

    void setLayerMode(bool enabled) { m_layer_mode = enabled; update(); }
    void setLayer(int index) { m_layer = std::clamp(index, 0, std::max(0, layerCount() - 1)); update(); }
    void setShowModel(bool enabled) { m_show_model = enabled; update(); }
    void setShowSupports(bool enabled) { m_show_supports = enabled; update(); }
    void resetCamera() { m_yaw = -35.0f; m_pitch = 58.0f; m_zoom = 1.0f; update(); }

protected:
    void initializeGL() override {
        initializeOpenGLFunctions();
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_MULTISAMPLE);

        static const char *vertex_shader = R"(
            #version 330 core
            layout(location = 0) in vec3 position;
            layout(location = 1) in vec3 normal;
            uniform mat4 mvp;
            uniform mat3 normal_matrix;
            out vec3 world_normal;
            void main() {
                gl_Position = mvp * vec4(position, 1.0);
                world_normal = normalize(normal_matrix * normal);
            }
        )";
        static const char *fragment_shader = R"(
            #version 330 core
            in vec3 world_normal;
            uniform vec3 base_colour;
            out vec4 fragment;
            void main() {
                vec3 light = normalize(vec3(-0.35, -0.45, 1.0));
                float value = 0.35 + 0.65 * max(dot(normalize(world_normal), light), 0.0);
                fragment = vec4(base_colour * value, 1.0);
            }
        )";
        if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertex_shader) ||
            !m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragment_shader) ||
            !m_program.link()) {
            QMessageBox::critical(this, "OpenGL error", m_program.log());
            return;
        }
        m_vao.create();
        m_model_vbo.create();
        m_model_vbo.bind();
        m_model_vbo.allocate(m_scene.model_vertices.constData(),
                             m_scene.model_vertices.size() * int(sizeof(float)));
        m_model_vbo.release();
        m_support_vbo.create();
        m_support_vbo.bind();
        m_support_vbo.allocate(m_scene.support_vertices.constData(),
                               m_scene.support_vertices.size() * int(sizeof(float)));
        m_support_vbo.release();
    }

    void resizeGL(int width, int height) override { glViewport(0, 0, width, height); }

    void paintGL() override {
        if (m_layer_mode) {
            drawLayer();
            return;
        }
        // QPainter's OpenGL paint engine is free to change the context state.
        // Restore every state relied on by the mesh pass when returning from
        // the 2D layer view; otherwise triangles are drawn in submission order
        // with no depth rejection and the front of the model looks transparent.
        const qreal pixel_ratio = devicePixelRatioF();
        glViewport(0, 0, int(width() * pixel_ratio), int(height() * pixel_ratio));
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_MULTISAMPLE);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glClearDepth(1.0);
        glClearColor(0.055f, 0.075f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (!m_program.isLinked())
            return;

        QVector3D center = (m_scene.minimum + m_scene.maximum) * 0.5f;
        float radius = std::max(1.0f, (m_scene.maximum - m_scene.minimum).length() * 0.5f);
        QMatrix4x4 projection;
        projection.perspective(35.0f, float(width()) / std::max(1, height()), radius * 0.005f,
                               radius * 20.0f);
        QMatrix4x4 view;
        view.lookAt(QVector3D(0.0f, -radius * 3.0f / m_zoom, radius * 1.8f / m_zoom),
                    QVector3D(), QVector3D(0.0f, 0.0f, 1.0f));
        QMatrix4x4 model;
        model.rotate(m_pitch - 58.0f, 1.0f, 0.0f, 0.0f);
        model.rotate(m_yaw + 35.0f, 0.0f, 0.0f, 1.0f);
        model.translate(-center);
        QMatrix4x4 model_view = view * model;
        QMatrix4x4 mvp = projection * model_view;

        m_program.bind();
        m_vao.bind();
        m_program.setUniformValue("mvp", mvp);
        m_program.setUniformValue("normal_matrix", model_view.normalMatrix());
        if (m_show_supports)
            drawBuffer(m_support_vbo, m_scene.support_vertices.size() / 6,
                       QVector3D(0.12f, 0.42f, 0.95f));
        if (m_show_model)
            drawBuffer(m_model_vbo, m_scene.model_vertices.size() / 6,
                       QVector3D(0.18f, 0.76f, 0.73f));
        m_vao.release();
        m_program.release();
    }

    void mousePressEvent(QMouseEvent *event) override { m_last_mouse = event->position(); }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (!m_layer_mode && (event->buttons() & Qt::LeftButton)) {
            QPointF delta = event->position() - m_last_mouse;
            m_yaw += float(delta.x()) * 0.5f;
            m_pitch = std::clamp(m_pitch + float(delta.y()) * 0.5f, -89.0f, 89.0f);
            update();
        }
        m_last_mouse = event->position();
    }

    void wheelEvent(QWheelEvent *event) override {
        m_zoom = std::clamp(m_zoom * std::exp(float(event->angleDelta().y()) / 900.0f), 0.15f, 8.0f);
        update();
    }

private:
    void drawBuffer(QOpenGLBuffer &buffer, int vertex_count, const QVector3D &colour) {
        if (!buffer.isCreated() || vertex_count == 0)
            return;
        buffer.bind();
        m_program.enableAttributeArray(0);
        m_program.enableAttributeArray(1);
        m_program.setAttributeBuffer(0, GL_FLOAT, 0, 3, 6 * int(sizeof(float)));
        m_program.setAttributeBuffer(1, GL_FLOAT, 3 * int(sizeof(float)), 3,
                                     6 * int(sizeof(float)));
        m_program.setUniformValue("base_colour", colour);
        glDrawArrays(GL_TRIANGLES, 0, vertex_count);
        buffer.release();
    }

    void drawLayer() {
        glClearColor(0.035f, 0.047f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const float margin = 32.0f;
        float scale = std::min((width() - 2.0f * margin) / m_scene.build_volume.x(),
                               (height() - 2.0f * margin) / m_scene.build_volume.y());
        auto mapPoint = [&](const QVector2D &point) {
            return QPointF(width() * 0.5f + point.x() * scale,
                           height() * 0.5f - point.y() * scale);
        };
        QRectF platform(width() * 0.5f - m_scene.build_volume.x() * scale * 0.5f,
                        height() * 0.5f - m_scene.build_volume.y() * scale * 0.5f,
                        m_scene.build_volume.x() * scale, m_scene.build_volume.y() * scale);
        painter.setPen(QPen(QColor(74, 94, 126), 1.0));
        painter.setBrush(QColor(20, 31, 48));
        painter.drawRect(platform);
        if (m_layer >= 0 && m_layer < m_scene.layers.size()) {
            QPainterPath path;
            path.setFillRule(Qt::WindingFill);
            for (const QVector<QVector2D> &contour : m_scene.layers[m_layer].contours) {
                if (contour.isEmpty())
                    continue;
                path.moveTo(mapPoint(contour[0]));
                for (qsizetype index = 1; index < contour.size(); ++index)
                    path.lineTo(mapPoint(contour[index]));
                path.closeSubpath();
            }
            painter.setPen(QPen(QColor(97, 232, 224), 1.0));
            painter.setBrush(QColor(49, 196, 189));
            painter.drawPath(path);
        }
    }

    SceneData m_scene;
    QOpenGLShaderProgram m_program;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer m_model_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_support_vbo{QOpenGLBuffer::VertexBuffer};
    QPointF m_last_mouse;
    float m_yaw = -35.0f;
    float m_pitch = 58.0f;
    float m_zoom = 1.0f;
    bool m_layer_mode = false;
    bool m_show_model = true;
    bool m_show_supports = true;
    int m_layer = 0;
};

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(SceneData scene, const QString &path) {
        setWindowTitle("Anycubic print viewer — " + QFileInfo(path).fileName());
        resize(1120, 720);
        auto *central = new QWidget;
        auto *layout = new QHBoxLayout(central);
        auto *view = new PrintView(std::move(scene));
        layout->addWidget(view, 1);

        auto *panel = new QWidget;
        panel->setFixedWidth(260);
        auto *controls = new QVBoxLayout(panel);
        auto *title = new QLabel("Print inspection");
        QFont title_font = title->font();
        title_font.setPointSize(title_font.pointSize() + 3);
        title_font.setBold(true);
        title->setFont(title_font);
        controls->addWidget(title);

        auto *mode = new QComboBox;
        mode->addItems({"3D model and supports", "Sliced layer"});
        controls->addWidget(mode);
        auto *show_model = new QCheckBox("Model");
        auto *show_supports = new QCheckBox("Supports");
        show_model->setChecked(true);
        show_supports->setChecked(true);
        controls->addWidget(show_model);
        controls->addWidget(show_supports);

        auto *layer_label = new QLabel;
        auto *layer_slider = new QSlider(Qt::Horizontal);
        layer_slider->setRange(0, std::max(0, view->layerCount() - 1));
        layer_slider->setEnabled(false);
        auto update_layer_label = [=](int layer) {
            layer_label->setText(QString("Layer %1 / %2 — Z %3 mm")
                                     .arg(layer + 1)
                                     .arg(view->layerCount())
                                     .arg(view->layerZ(layer), 0, 'f', 3));
        };
        update_layer_label(0);
        controls->addWidget(layer_label);
        controls->addWidget(layer_slider);

        auto *reset = new QPushButton("Reset camera");
        controls->addWidget(reset);
        auto *help = new QLabel(
            "3D: drag with the left mouse button to rotate; use the wheel to zoom.\n\n"
            "Layer mode displays the exact final polygons written to PWSZ.");
        help->setWordWrap(true);
        controls->addWidget(help);
        controls->addStretch(1);
        layout->addWidget(panel);
        setCentralWidget(central);

        connect(mode, &QComboBox::currentIndexChanged, this, [=](int index) {
            bool layers = index == 1;
            view->setLayerMode(layers);
            layer_slider->setEnabled(layers);
            show_model->setEnabled(!layers);
            show_supports->setEnabled(!layers);
        });
        connect(layer_slider, &QSlider::valueChanged, this, [=](int layer) {
            view->setLayer(layer);
            update_layer_label(layer);
        });
        connect(show_model, &QCheckBox::toggled, view, &PrintView::setShowModel);
        connect(show_supports, &QCheckBox::toggled, view, &PrintView::setShowSupports);
        connect(reset, &QPushButton::clicked, view, &PrintView::resetCamera);
    }
};

int main(int argc, char **argv) {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication application(argc, argv);
    if (argc != 2) {
        QMessageBox::critical(nullptr, "Usage", "anycubic-viewer SCENE.acv");
        return 2;
    }
    SceneData scene;
    QString error;
    if (!loadScene(QString::fromLocal8Bit(argv[1]), scene, error)) {
        QMessageBox::critical(nullptr, "Cannot open scene", error);
        return 1;
    }
    MainWindow window(std::move(scene), QString::fromLocal8Bit(argv[1]));
    window.show();
    return application.exec();
}
