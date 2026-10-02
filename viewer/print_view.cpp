#include "print_view.hpp"

#include <QMatrix4x4>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

PrintView::PrintView(QWidget *parent) : QOpenGLWidget(parent) {
    setMinimumSize(640, 520);
    setFocusPolicy(Qt::StrongFocus);
    resetCamera();
}

PrintView::~PrintView() {
    if (context()) {
        makeCurrent();
        m_model_vbo.destroy();
        m_support_vbo.destroy();
        m_platform_vbo.destroy();
        m_vao.destroy();
        doneCurrent();
    }
}

int PrintView::layerCount() const { return m_layers.size(); }

float PrintView::layerZ(int index) const {
    return index >= 0 && index < m_layers.size() ? m_layers[index].z : 0.0f;
}

void PrintView::setLayerMode(bool enabled) {
    m_layer_mode = enabled;
    update();
}

void PrintView::setLayer(int index) {
    m_layer = std::clamp(index, 0, std::max(0, layerCount() - 1));
    update();
}

void PrintView::setShowModel(bool enabled) {
    m_show_model = enabled;
    update();
}

void PrintView::setShowSupports(bool enabled) {
    m_show_supports = enabled;
    update();
}

void PrintView::setBuildVolume(const QVector3D &volume) {
    m_build_volume = volume;
    makePlatform();
    uploadIfReady();
    update();
}

void PrintView::setLayers(const QVector<LayerData> &layers) {
    m_layers = layers;
    m_layer = std::clamp(m_layer, 0, std::max(0, layerCount() - 1));
    update();
}

void PrintView::setModelScenes(const std::vector<std::unique_ptr<ModelState>> &models,
                               int selected) {
    m_model_cpu.clear();
    m_support_cpu.clear();
    m_model_ranges.clear();
    m_support_ranges.clear();
    bool has_bounds = false;
    for (int index = 0; index < static_cast<int>(models.size()); ++index) {
        const SceneData &scene = models[index]->visual;
        Range model_range;
        model_range.first = m_model_cpu.size() / 6;
        model_range.count = scene.model_vertices.size() / 6;
        model_range.selected = index == selected;
        m_model_cpu += scene.model_vertices;
        m_model_ranges.push_back(model_range);

        Range support_range;
        support_range.first = m_support_cpu.size() / 6;
        support_range.count = scene.support_vertices.size() / 6;
        support_range.selected = index == selected;
        m_support_cpu += scene.support_vertices;
        m_support_ranges.push_back(support_range);

        if (!scene.model_vertices.isEmpty() || !scene.support_vertices.isEmpty()) {
            if (!has_bounds) {
                m_minimum = scene.minimum;
                m_maximum = scene.maximum;
                has_bounds = true;
            } else {
                m_minimum.setX(std::min(m_minimum.x(), scene.minimum.x()));
                m_minimum.setY(std::min(m_minimum.y(), scene.minimum.y()));
                m_minimum.setZ(std::min(m_minimum.z(), scene.minimum.z()));
                m_maximum.setX(std::max(m_maximum.x(), scene.maximum.x()));
                m_maximum.setY(std::max(m_maximum.y(), scene.maximum.y()));
                m_maximum.setZ(std::max(m_maximum.z(), scene.maximum.z()));
            }
        }
    }
    if (!has_bounds) {
        m_minimum = QVector3D(-1.0f, -1.0f, 0.0f);
        m_maximum = QVector3D(1.0f, 1.0f, 1.0f);
    }
    uploadIfReady();
    update();
}

void PrintView::resetCamera() {
    m_yaw = -35.0f;
    m_pitch = 58.0f;
    m_zoom = 1.0f;
    m_pan = QVector2D();
    update();
}

void PrintView::initializeGL() {
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
        uniform float light_amount;
        out vec4 fragment;
        void main() {
            vec3 light = normalize(vec3(-0.35, -0.45, 1.0));
            float shaded = 0.35 + 0.65 * max(dot(normalize(world_normal), light), 0.0);
            float value = mix(1.0, shaded, light_amount);
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
    m_support_vbo.create();
    m_platform_vbo.create();
    makePlatform();
    uploadBuffers();
}

void PrintView::resizeGL(int width, int height) { glViewport(0, 0, width, height); }

void PrintView::paintGL() {
    if (m_layer_mode) {
        drawLayer();
        return;
    }
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

    const float plate_radius = std::max(m_build_volume.x(), m_build_volume.y()) * 0.58f;
    const float model_radius = std::max(1.0f, (m_maximum - m_minimum).length() * 0.55f);
    const float radius = std::max(plate_radius, model_radius);
    const QVector3D centre(0.0f, 0.0f,
                           std::max(0.0f, (m_minimum.z() + m_maximum.z()) * 0.5f));
    QMatrix4x4 projection;
    projection.perspective(35.0f, float(width()) / std::max(1, height()),
                           std::max(0.01f, radius * 0.005f), radius * 30.0f);
    QMatrix4x4 view;
    view.lookAt(QVector3D(0.0f, -radius * 3.0f / m_zoom, radius * 1.8f / m_zoom),
                QVector3D(), QVector3D(0.0f, 0.0f, 1.0f));
    QMatrix4x4 world;
    world.rotate(m_pitch - 58.0f, 1.0f, 0.0f, 0.0f);
    world.rotate(m_yaw + 35.0f, 0.0f, 0.0f, 1.0f);
    world.translate(-centre);
    const QMatrix4x4 model_view = view * world;
    QMatrix4x4 screen_pan;
    screen_pan.translate(m_pan.x(), m_pan.y(), 0.0f);
    const QMatrix4x4 mvp = projection * screen_pan * model_view;

    m_program.bind();
    m_vao.bind();
    m_program.setUniformValue("mvp", mvp);
    m_program.setUniformValue("normal_matrix", model_view.normalMatrix());
    m_program.setUniformValue("light_amount", 0.0f);
    glLineWidth(1.25f);
    drawRange(m_platform_vbo, 0, m_platform_cpu.size() / 6,
              QVector3D(0.38f, 0.48f, 0.62f), GL_LINES);
    m_program.setUniformValue("light_amount", 1.0f);
    if (m_show_supports) {
        for (const Range &range : m_support_ranges) {
            drawRange(m_support_vbo, range.first, range.count,
                      range.selected ? QVector3D(0.95f, 0.55f, 0.12f)
                                     : QVector3D(0.12f, 0.42f, 0.95f),
                      GL_TRIANGLES);
        }
    }
    if (m_show_model) {
        for (const Range &range : m_model_ranges) {
            drawRange(m_model_vbo, range.first, range.count,
                      range.selected ? QVector3D(1.0f, 0.62f, 0.16f)
                                     : QVector3D(0.18f, 0.76f, 0.73f),
                      GL_TRIANGLES);
        }
    }
    m_vao.release();
    m_program.release();
}

void PrintView::mousePressEvent(QMouseEvent *event) { m_last_mouse = event->position(); }

void PrintView::mouseMoveEvent(QMouseEvent *event) {
    if (!m_layer_mode) {
        const QPointF delta = event->position() - m_last_mouse;
        const bool pan = (event->buttons() & (Qt::MiddleButton | Qt::RightButton)) ||
                         ((event->buttons() & Qt::LeftButton) &&
                          (event->modifiers() & Qt::ShiftModifier));
        if (pan) {
            const float radius = std::max(m_build_volume.x(), m_build_volume.y()) * 0.58f;
            const float scale = 2.0f * radius / (std::max(1, height()) * m_zoom);
            m_pan += QVector2D(float(delta.x()) * scale, float(-delta.y()) * scale);
            update();
        } else if (event->buttons() & Qt::LeftButton) {
            m_yaw += float(delta.x()) * 0.5f;
            m_pitch = std::clamp(m_pitch + float(delta.y()) * 0.5f, -89.0f, 89.0f);
            update();
        }
    }
    m_last_mouse = event->position();
}

void PrintView::wheelEvent(QWheelEvent *event) {
    m_zoom = std::clamp(m_zoom * std::exp(float(event->angleDelta().y()) / 900.0f),
                        0.15f, 8.0f);
    update();
}

void PrintView::uploadIfReady() {
    if (!isValid())
        return;
    makeCurrent();
    uploadBuffers();
    doneCurrent();
}

void PrintView::uploadBuffer(QOpenGLBuffer &buffer, const QVector<float> &values) {
    buffer.bind();
    buffer.allocate(values.isEmpty() ? nullptr : values.constData(),
                    values.size() * int(sizeof(float)));
    buffer.release();
}

void PrintView::uploadBuffers() {
    if (!m_model_vbo.isCreated())
        return;
    uploadBuffer(m_model_vbo, m_model_cpu);
    uploadBuffer(m_support_vbo, m_support_cpu);
    uploadBuffer(m_platform_vbo, m_platform_cpu);
}

void PrintView::appendPlatformVertex(const QVector3D &point) {
    m_platform_cpu << point.x() << point.y() << point.z();
    m_platform_cpu << 0.0f << 0.0f << 1.0f;
}

void PrintView::appendPlatformEdge(const QVector3D &first, const QVector3D &second) {
    appendPlatformVertex(first);
    appendPlatformVertex(second);
}

void PrintView::makePlatform() {
    m_platform_cpu.clear();
    const float hx = m_build_volume.x() * 0.5f;
    const float hy = m_build_volume.y() * 0.5f;
    const float z = m_build_volume.z();
    const QVector3D bottom[] = {{-hx, -hy, 0}, {hx, -hy, 0}, {hx, hy, 0}, {-hx, hy, 0}};
    const QVector3D top[] = {{-hx, -hy, z}, {hx, -hy, z}, {hx, hy, z}, {-hx, hy, z}};
    for (int index = 0; index < 4; ++index) {
        const int next = (index + 1) % 4;
        appendPlatformEdge(bottom[index], bottom[next]);
        appendPlatformEdge(top[index], top[next]);
        appendPlatformEdge(bottom[index], top[index]);
    }
    for (int division = 1; division < 10; ++division) {
        const float x = -hx + 2.0f * hx * division / 10.0f;
        const float y = -hy + 2.0f * hy * division / 10.0f;
        appendPlatformEdge(QVector3D(x, -hy, 0), QVector3D(x, hy, 0));
        appendPlatformEdge(QVector3D(-hx, y, 0), QVector3D(hx, y, 0));
    }
}

void PrintView::drawRange(QOpenGLBuffer &buffer, int first, int count,
                          const QVector3D &colour, GLenum primitive) {
    if (!buffer.isCreated() || count == 0)
        return;
    buffer.bind();
    m_program.enableAttributeArray(0);
    m_program.enableAttributeArray(1);
    m_program.setAttributeBuffer(0, GL_FLOAT, 0, 3, 6 * int(sizeof(float)));
    m_program.setAttributeBuffer(1, GL_FLOAT, 3 * int(sizeof(float)), 3,
                                 6 * int(sizeof(float)));
    m_program.setUniformValue("base_colour", colour);
    glDrawArrays(primitive, first, count);
    buffer.release();
}

void PrintView::drawLayer() {
    glClearColor(0.035f, 0.047f, 0.07f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const float margin = 32.0f;
    const float scale = std::min((width() - 2.0f * margin) / m_build_volume.x(),
                                 (height() - 2.0f * margin) / m_build_volume.y());
    auto mapPoint = [&](const QVector2D &point) {
        return QPointF(width() * 0.5f + point.x() * scale,
                       height() * 0.5f - point.y() * scale);
    };
    const QRectF platform(width() * 0.5f - m_build_volume.x() * scale * 0.5f,
                          height() * 0.5f - m_build_volume.y() * scale * 0.5f,
                          m_build_volume.x() * scale, m_build_volume.y() * scale);
    painter.setPen(QPen(QColor(74, 94, 126), 1.0));
    painter.setBrush(QColor(20, 31, 48));
    painter.drawRect(platform);
    if (m_layer >= 0 && m_layer < m_layers.size()) {
        QPainterPath path;
        path.setFillRule(Qt::WindingFill);
        for (const QVector<QVector2D> &contour : m_layers[m_layer].contours) {
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

