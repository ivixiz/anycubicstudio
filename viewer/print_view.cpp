#include "print_view.hpp"

#include <QMatrix4x4>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>
#include <QVector4D>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

PrintView::PrintView(QWidget *parent) : QOpenGLWidget(parent) {
    setMinimumSize(640, 520);
    setFocusPolicy(Qt::StrongFocus);
    resetCamera();
}

PrintView::~PrintView() {
    if (!context())
        return;
    makeCurrent();
    m_model_vbo.destroy();
    m_support_vbo.destroy();
    m_artwork_vbo.destroy();
    m_platform_vbo.destroy();
    m_gizmo_vbo.destroy();
    m_vao.destroy();
    doneCurrent();
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
    makeGizmo();
    uploadIfReady();
    update();
}

void PrintView::setLayers(const QVector<LayerData> &layers) {
    m_layers = layers;
    m_layer = std::clamp(m_layer, 0, std::max(0, layerCount() - 1));
    update();
}

void PrintView::setGizmoMode(GizmoMode mode) {
    m_gizmo_mode = mode;
    update();
}

void PrintView::setRotationZOnly(bool enabled) { m_rotation_z_only = enabled; }

void PrintView::setMinimumSelectedScale(float scale) {
    m_minimum_selected_scale = std::max(1, int(std::lround(scale)));
}

void PrintView::setSelectionHandler(SelectionHandler handler) {
    m_selection_handler = std::move(handler);
}

void PrintView::setTransformHandler(TransformHandler handler) {
    m_transform_handler = std::move(handler);
}

void PrintView::setTransformFinishedHandler(TransformFinishedHandler handler) {
    m_transform_finished_handler = std::move(handler);
}

void PrintView::appendLineVertex(QVector<float> &target, const QVector3D &point) {
    target << point.x() << point.y() << point.z() << 0.0f << 0.0f << 1.0f;
}

void PrintView::appendLine(QVector<float> &target, const QVector3D &first,
                           const QVector3D &second) {
    appendLineVertex(target, first);
    appendLineVertex(target, second);
}

void PrintView::setObjects(const std::vector<std::unique_ptr<SceneObject>> &objects) {
    m_model_cpu.clear();
    m_support_cpu.clear();
    m_artwork_cpu.clear();
    m_model_ranges.clear();
    m_support_ranges.clear();
    m_artwork_ranges.clear();
    m_pick_bounds.assign(objects.size(), {});
    bool has_bounds = false;
    QVector3D selected_minimum;
    QVector3D selected_maximum;
    bool has_selected_bounds = false;
    for (int index = 0; index < static_cast<int>(objects.size()); ++index) {
        const SceneObject &object = *objects[index];
        if (!object.visible)
            continue;
        const SceneData &scene = object.visual;
        Range model_range{int(m_model_cpu.size() / 6), int(scene.model_vertices.size() / 6),
                          index, object.selected};
        m_model_cpu += scene.model_vertices;
        m_model_ranges.push_back(model_range);
        Range support_range{int(m_support_cpu.size() / 6), int(scene.support_vertices.size() / 6),
                            index, object.selected};
        m_support_cpu += scene.support_vertices;
        m_support_ranges.push_back(support_range);

        const int artwork_first = m_artwork_cpu.size() / 6;
        for (const LayerData &layer : scene.layers) {
            for (const QVector<QVector2D> &contour : layer.contours) {
                if (contour.size() < 2)
                    continue;
                for (qsizetype point = 0; point < contour.size(); ++point) {
                    const QVector2D first = contour[point];
                    const QVector2D second = contour[(point + 1) % contour.size()];
                    appendLine(m_artwork_cpu, QVector3D(first.x(), first.y(), layer.z),
                               QVector3D(second.x(), second.y(), layer.z));
                }
            }
        }
        m_artwork_ranges.push_back(
            {artwork_first, int(m_artwork_cpu.size() / 6) - artwork_first, index,
             object.selected});

        const bool object_has_bounds = !scene.model_vertices.isEmpty() ||
                                       !scene.support_vertices.isEmpty() ||
                                       !scene.layers.isEmpty();
        if (!object_has_bounds)
            continue;
        m_pick_bounds[index] = {scene.minimum, scene.maximum, true, object.selected};
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
        if (object.selected) {
            if (!has_selected_bounds) {
                selected_minimum = scene.minimum;
                selected_maximum = scene.maximum;
                has_selected_bounds = true;
            } else {
                selected_minimum.setX(std::min(selected_minimum.x(), scene.minimum.x()));
                selected_minimum.setY(std::min(selected_minimum.y(), scene.minimum.y()));
                selected_minimum.setZ(std::min(selected_minimum.z(), scene.minimum.z()));
                selected_maximum.setX(std::max(selected_maximum.x(), scene.maximum.x()));
                selected_maximum.setY(std::max(selected_maximum.y(), scene.maximum.y()));
                selected_maximum.setZ(std::max(selected_maximum.z(), scene.maximum.z()));
            }
        }
    }
    if (!has_bounds) {
        m_minimum = QVector3D(-1.0f, -1.0f, 0.0f);
        m_maximum = QVector3D(1.0f, 1.0f, 1.0f);
    }
    m_gizmo_center = has_selected_bounds ? (selected_minimum + selected_maximum) * 0.5f
                                          : QVector3D();
    makeGizmo();
    uploadIfReady();
    update();
}

void PrintView::resetCamera() {
    m_yaw = -35.0f;
    m_pitch = 32.0f;
    m_zoom = 1.0f;
    m_pan = QVector2D();
    fitLayer();
    update();
}

void PrintView::fitLayer() {
    m_layer_zoom = 1.0f;
    m_layer_pan = QVector2D();
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
            fragment = vec4(base_colour * mix(1.0, shaded, light_amount), 1.0);
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
    m_artwork_vbo.create();
    m_platform_vbo.create();
    m_gizmo_vbo.create();
    makePlatform();
    makeGizmo();
    uploadBuffers();
}

void PrintView::resizeGL(int width, int height) { glViewport(0, 0, width, height); }

QMatrix4x4 PrintView::currentMvp() const {
    const float plate_radius = std::max(m_build_volume.x(), m_build_volume.y()) * 0.58f;
    const float model_radius = std::max(1.0f, (m_maximum - m_minimum).length() * 0.55f);
    const float radius = std::max(plate_radius, model_radius);
    const QVector3D centre(0.0f, 0.0f,
                           std::max(0.0f, (m_minimum.z() + m_maximum.z()) * 0.5f));
    QMatrix4x4 projection;
    projection.perspective(35.0f, float(width()) / std::max(1, height()),
                           std::max(0.01f, radius * 0.005f), radius * 30.0f);
    const float yaw = qDegreesToRadians(m_yaw);
    const float pitch = qDegreesToRadians(m_pitch);
    const QVector3D eye_direction(std::sin(yaw) * std::cos(pitch),
                                  -std::cos(yaw) * std::cos(pitch), std::sin(pitch));
    const QVector3D forward = -eye_direction;
    const QVector3D right(std::cos(yaw), std::sin(yaw), 0.0f);
    const QVector3D up = QVector3D::crossProduct(right, forward).normalized();
    const QVector3D target = centre - right * m_pan.x() - up * m_pan.y();
    const float distance = radius * 3.5f / m_zoom;
    QMatrix4x4 view;
    view.lookAt(target + eye_direction * distance, target, up);
    return projection * view;
}

QVector3D PrintView::cameraForward() const {
    const float yaw = qDegreesToRadians(m_yaw);
    const float pitch = qDegreesToRadians(m_pitch);
    return QVector3D(-std::sin(yaw) * std::cos(pitch),
                     std::cos(yaw) * std::cos(pitch), -std::sin(pitch));
}

float PrintView::worldUnitsPerPixel() const {
    const float plate_radius = std::max(m_build_volume.x(), m_build_volume.y()) * 0.58f;
    const float model_radius = std::max(1.0f, (m_maximum - m_minimum).length() * 0.55f);
    const float distance = std::max(plate_radius, model_radius) * 3.5f / m_zoom;
    return 2.0f * distance * std::tan(qDegreesToRadians(35.0f) * 0.5f) /
           std::max(1, height());
}

QPointF PrintView::projectPoint(const QVector3D &point, const QMatrix4x4 &mvp) const {
    const QVector4D clip = mvp * QVector4D(point, 1.0f);
    if (std::abs(clip.w()) < 1.0e-7f)
        return QPointF(width() * 0.5, height() * 0.5);
    const QVector3D ndc = clip.toVector3DAffine();
    return QPointF((ndc.x() * 0.5f + 0.5f) * width(),
                   (0.5f - ndc.y() * 0.5f) * height());
}

bool PrintView::unprojectToDragPlane(const QPointF &position, QVector3D &point) const {
    const float x = float(position.x() * 2.0 / std::max(1, width()) - 1.0);
    const float y = float(1.0 - position.y() * 2.0 / std::max(1, height()));
    QVector4D near_point = m_drag_inverse_mvp * QVector4D(x, y, -1.0f, 1.0f);
    QVector4D far_point = m_drag_inverse_mvp * QVector4D(x, y, 1.0f, 1.0f);
    if (std::abs(near_point.w()) < 1.0e-7f || std::abs(far_point.w()) < 1.0e-7f)
        return false;
    near_point /= near_point.w();
    far_point /= far_point.w();
    const QVector3D origin = near_point.toVector3D();
    const QVector3D direction = (far_point - near_point).toVector3D().normalized();
    const float denominator = QVector3D::dotProduct(direction, m_drag_plane_normal);
    if (std::abs(denominator) < 1.0e-6f)
        return false;
    const float distance = QVector3D::dotProduct(m_drag_plane_point - origin,
                                                  m_drag_plane_normal) /
                           denominator;
    point = origin + direction * distance;
    return std::isfinite(point.x()) && std::isfinite(point.y()) && std::isfinite(point.z());
}

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
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDisable(GL_BLEND);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glClearDepth(1.0);
    glClearColor(0.055f, 0.075f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!m_program.isLinked())
        return;

    const QMatrix4x4 mvp = currentMvp();
    m_program.bind();
    m_vao.bind();
    m_program.setUniformValue("mvp", mvp);
    m_program.setUniformValue("normal_matrix", QMatrix3x3());
    m_program.setUniformValue("light_amount", 0.0f);
    glDisable(GL_CULL_FACE);
    glLineWidth(1.25f);
    drawRange(m_platform_vbo, 0, m_platform_cpu.size() / 6,
              QVector3D(0.38f, 0.48f, 0.62f), GL_LINES);
    if (m_show_model)
        for (const Range &range : m_artwork_ranges) {
            drawRange(m_artwork_vbo, range.first, range.count,
                      range.selected ? QVector3D(1.0f, 0.62f, 0.16f)
                                     : QVector3D(0.25f, 0.95f, 0.62f),
                      GL_LINES);
        }
    // Keep triangle meshes two-sided. Imported STL winding is not guaranteed,
    // and depth writes already provide opaque surface occlusion.
    glDisable(GL_CULL_FACE);
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
    glDisable(GL_CULL_FACE);
    glLineWidth(3.0f);
    drawRange(m_gizmo_vbo, 0, 2, QVector3D(0.95f, 0.2f, 0.2f), GL_LINES);
    drawRange(m_gizmo_vbo, 2, 2, QVector3D(0.2f, 0.9f, 0.3f), GL_LINES);
    drawRange(m_gizmo_vbo, 4, 2, QVector3D(0.25f, 0.45f, 1.0f), GL_LINES);
    m_vao.release();
    m_program.release();
}

int PrintView::pickObject(const QPointF &position) const {
    const QMatrix4x4 mvp = currentMvp();
    int best = -1;
    float best_score = std::numeric_limits<float>::max();
    for (int index = 0; index < static_cast<int>(m_pick_bounds.size()); ++index) {
        const PickBounds &bounds = m_pick_bounds[index];
        if (!bounds.valid)
            continue;
        float min_x = std::numeric_limits<float>::max();
        float min_y = min_x;
        float max_x = -min_x;
        float max_y = -min_x;
        float depth = 0.0f;
        int projected = 0;
        for (int corner = 0; corner < 8; ++corner) {
            const QVector3D point((corner & 1) ? bounds.maximum.x() : bounds.minimum.x(),
                                  (corner & 2) ? bounds.maximum.y() : bounds.minimum.y(),
                                  (corner & 4) ? bounds.maximum.z() : bounds.minimum.z());
            const QVector4D clip = mvp * QVector4D(point, 1.0f);
            if (clip.w() <= 0.0f)
                continue;
            const QVector3D ndc = clip.toVector3DAffine();
            const float x = (ndc.x() * 0.5f + 0.5f) * width();
            const float y = (0.5f - ndc.y() * 0.5f) * height();
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
            depth += ndc.z();
            ++projected;
        }
        if (projected == 0 || position.x() < min_x - 5.0 || position.x() > max_x + 5.0 ||
            position.y() < min_y - 5.0 || position.y() > max_y + 5.0)
            continue;
        const float area = std::max(1.0f, (max_x - min_x) * (max_y - min_y));
        const float score = depth / projected + std::log(area) * 0.001f;
        if (score < best_score) {
            best_score = score;
            best = index;
        }
    }
    return best;
}

void PrintView::mousePressEvent(QMouseEvent *event) {
    m_last_mouse = m_press_mouse = event->position();
    m_moved = false;
    if (m_layer_mode)
        return;
    m_pressed_object = event->button() == Qt::LeftButton ? pickObject(event->position()) : -1;
    if (event->button() == Qt::LeftButton && m_selection_handler)
        m_selection_handler(m_pressed_object, event->modifiers());
    m_dragging_object = m_pressed_object >= 0 && event->button() == Qt::LeftButton &&
                        !(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier));
    if (!m_dragging_object)
        return;
    bool invertible = false;
    const QMatrix4x4 mvp = currentMvp();
    m_drag_inverse_mvp = mvp.inverted(&invertible);
    if (!invertible) {
        m_dragging_object = false;
        return;
    }
    m_drag_plane_point = m_gizmo_center;
    m_drag_plane_normal = m_gizmo_mode == GizmoMode::Move
                              ? QVector3D(0.0f, 0.0f, 1.0f)
                              : cameraForward().normalized();
    m_drag_unproject_valid = unprojectToDragPlane(event->position(), m_drag_start_world);
    if (m_gizmo_mode != GizmoMode::Move)
        m_drag_unproject_valid = true;
    m_drag_emitted_move = QVector3D();
    m_gizmo_screen_center = projectPoint(m_gizmo_center, mvp);
    const QPointF radial(event->position().x() - m_gizmo_screen_center.x(),
                         m_gizmo_screen_center.y() - event->position().y());
    m_drag_last_angle = std::atan2(float(radial.y()), float(radial.x()));
    m_drag_angle_total = 0.0f;
    m_drag_emitted_angle = 0.0f;
    m_drag_start_radius = float(std::hypot(radial.x(), radial.y()));
    m_drag_emitted_scale_steps = 0;
    if (m_rotation_z_only) {
        m_drag_rotation_axis = QVector3D(0.0f, 0.0f, 1.0f);
    } else {
        const QVector3D toward_camera = -m_drag_plane_normal;
        const QVector3D absolute(std::abs(toward_camera.x()), std::abs(toward_camera.y()),
                                 std::abs(toward_camera.z()));
        if (absolute.x() >= absolute.y() && absolute.x() >= absolute.z())
            m_drag_rotation_axis = QVector3D(std::copysign(1.0f, toward_camera.x()), 0, 0);
        else if (absolute.y() >= absolute.z())
            m_drag_rotation_axis = QVector3D(0, std::copysign(1.0f, toward_camera.y()), 0);
        else
            m_drag_rotation_axis = QVector3D(0, 0, std::copysign(1.0f, toward_camera.z()));
    }
}

void PrintView::mouseMoveEvent(QMouseEvent *event) {
    const QPointF delta = event->position() - m_last_mouse;
    m_moved = m_moved || (event->position() - m_press_mouse).manhattanLength() > 3.0;
    if (m_layer_mode) {
        if (event->buttons() & (Qt::LeftButton | Qt::MiddleButton | Qt::RightButton)) {
            m_layer_pan += QVector2D(float(delta.x()), float(delta.y()));
            update();
        }
    } else if (m_dragging_object && (event->buttons() & Qt::LeftButton) &&
               m_transform_handler) {
        if (m_gizmo_mode == GizmoMode::Move) {
            QVector3D world;
            QVector3D total;
            if (m_drag_unproject_valid && unprojectToDragPlane(event->position(), world)) {
                total = world - m_drag_start_world;
            } else {
                const QPointF screen = event->position() - m_press_mouse;
                const float yaw = qDegreesToRadians(m_yaw);
                const QVector3D right(std::cos(yaw), std::sin(yaw), 0.0f);
                QVector3D forward = cameraForward();
                forward.setZ(0.0f);
                if (!forward.isNull())
                    forward.normalize();
                total = (right * float(screen.x()) + forward * float(-screen.y())) *
                        worldUnitsPerPixel();
            }
            total.setZ(0.0f);
            const QVector3D snapped(std::round(total.x()), std::round(total.y()), 0.0f);
            const QVector3D increment = snapped - m_drag_emitted_move;
            if (!increment.isNull()) {
                m_transform_handler(m_gizmo_mode, increment);
                m_drag_emitted_move = snapped;
            }
        } else if (m_gizmo_mode == GizmoMode::Rotate) {
            const QPointF radial(event->position().x() - m_gizmo_screen_center.x(),
                                 m_gizmo_screen_center.y() - event->position().y());
            const float radius = float(std::hypot(radial.x(), radial.y()));
            float angle_increment = 0.0f;
            if (radius >= 10.0f && m_drag_start_radius >= 10.0f) {
                const float angle = std::atan2(float(radial.y()), float(radial.x()));
                angle_increment = std::remainder(angle - m_drag_last_angle,
                                                 2.0f * float(M_PI));
                m_drag_last_angle = angle;
            } else {
                angle_increment = float(delta.x()) * qDegreesToRadians(0.5f);
            }
            m_drag_angle_total += qRadiansToDegrees(angle_increment);
            const float snapped = std::round(m_drag_angle_total / 15.0f) * 15.0f;
            const float increment = snapped - m_drag_emitted_angle;
            if (std::abs(increment) >= 0.1f) {
                m_transform_handler(m_gizmo_mode, m_drag_rotation_axis * increment);
                m_drag_emitted_angle = snapped;
            }
        } else {
            const QPointF radial(event->position().x() - m_gizmo_screen_center.x(),
                                 m_gizmo_screen_center.y() - event->position().y());
            const float radius = float(std::hypot(radial.x(), radial.y()));
            const int raw_steps = std::lround((radius - m_drag_start_radius) / 28.0f);
            const int steps = std::max(1 - m_minimum_selected_scale, raw_steps);
            const int increment = steps - m_drag_emitted_scale_steps;
            if (increment != 0) {
                m_transform_handler(m_gizmo_mode,
                                    QVector3D(increment, increment, increment));
                m_drag_emitted_scale_steps = steps;
            }
        }
    } else {
        const bool pan = (event->buttons() & (Qt::MiddleButton | Qt::RightButton)) ||
                         ((event->buttons() & Qt::LeftButton) &&
                          (event->modifiers() & Qt::ShiftModifier));
        if (pan) {
            const float scale = worldUnitsPerPixel();
            m_pan += QVector2D(float(delta.x()) * scale, float(-delta.y()) * scale);
            update();
        } else if ((event->buttons() & Qt::LeftButton) && m_pressed_object < 0) {
            m_yaw += float(delta.x()) * 0.5f;
            m_pitch = std::clamp(m_pitch + float(delta.y()) * 0.5f, -89.9f, 89.9f);
            update();
        }
    }
    m_last_mouse = event->position();
}

void PrintView::mouseReleaseEvent(QMouseEvent *) {
    const bool finished_transform = m_dragging_object;
    m_dragging_object = false;
    m_pressed_object = -1;
    if (finished_transform && m_transform_finished_handler)
        m_transform_finished_handler();
}

void PrintView::wheelEvent(QWheelEvent *event) {
    if (m_layer_mode) {
        const float previous = m_layer_zoom;
        const float next = std::clamp(
            previous * std::exp(float(event->angleDelta().y()) / 900.0f), 0.1f, 40.0f);
        if (next != previous) {
            const QVector2D cursor(float(event->position().x() - width() * 0.5),
                                   float(event->position().y() - height() * 0.5));
            const float ratio = next / previous;
            m_layer_pan = cursor - (cursor - m_layer_pan) * ratio;
            m_layer_zoom = next;
        }
    } else {
        m_zoom = std::clamp(m_zoom * std::exp(float(event->angleDelta().y()) / 900.0f),
                            0.15f, 8.0f);
    }
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
    uploadBuffer(m_artwork_vbo, m_artwork_cpu);
    uploadBuffer(m_platform_vbo, m_platform_cpu);
    uploadBuffer(m_gizmo_vbo, m_gizmo_cpu);
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
        appendLine(m_platform_cpu, bottom[index], bottom[next]);
        appendLine(m_platform_cpu, top[index], top[next]);
        appendLine(m_platform_cpu, bottom[index], top[index]);
    }
    for (int division = 1; division < 10; ++division) {
        const float x = -hx + 2.0f * hx * division / 10.0f;
        const float y = -hy + 2.0f * hy * division / 10.0f;
        appendLine(m_platform_cpu, QVector3D(x, -hy, 0), QVector3D(x, hy, 0));
        appendLine(m_platform_cpu, QVector3D(-hx, y, 0), QVector3D(hx, y, 0));
    }
}

void PrintView::makeGizmo() {
    m_gizmo_cpu.clear();
    if (std::none_of(m_pick_bounds.begin(), m_pick_bounds.end(),
                     [](const PickBounds &bounds) { return bounds.selected; }))
        return;
    const float length = std::min(m_build_volume.x(), m_build_volume.y()) * 0.08f;
    appendLine(m_gizmo_cpu, m_gizmo_center, m_gizmo_center + QVector3D(length, 0, 0));
    appendLine(m_gizmo_cpu, m_gizmo_center, m_gizmo_center + QVector3D(0, length, 0));
    appendLine(m_gizmo_cpu, m_gizmo_center, m_gizmo_center + QVector3D(0, 0, length));
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
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.035f, 0.047f, 0.07f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const float margin = 32.0f;
    const float fit = std::min((width() - 2.0f * margin) / m_build_volume.x(),
                               (height() - 2.0f * margin) / m_build_volume.y());
    const float scale = fit * m_layer_zoom;
    auto mapPoint = [&](const QVector2D &point) {
        return QPointF(width() * 0.5f + m_layer_pan.x() + point.x() * scale,
                       height() * 0.5f + m_layer_pan.y() - point.y() * scale);
    };
    const QRectF platform(width() * 0.5f + m_layer_pan.x() -
                              m_build_volume.x() * scale * 0.5f,
                          height() * 0.5f + m_layer_pan.y() -
                              m_build_volume.y() * scale * 0.5f,
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
