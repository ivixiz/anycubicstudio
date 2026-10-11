#include "print_view.hpp"

#include <QMatrix4x4>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QLineF>
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
    m_slice_vbo.destroy();
    m_slice_fill_vbo.destroy();
    m_vao.destroy();
    doneCurrent();
}

int PrintView::layerCount() const { return m_layers.size(); }

float PrintView::layerZ(int index) const {
    return index >= 0 && index < m_layers.size() ? m_layers[index].z : 0.0f;
}

void PrintView::setSliceOverlay(bool enabled) {
    m_slice_overlay = enabled && !m_layers.isEmpty();
    update();
}

void PrintView::setLayer(int index) {
    m_layer = std::clamp(index, 0, std::max(0, layerCount() - 1));
    makeSliceOverlay();
    uploadSliceIfReady();
    update();
}

void PrintView::setBuildVolume(const QVector3D &volume) {
    m_build_volume = volume;
    makePlatform();
    makeGizmo();
    uploadIfReady();
    update();
}

void PrintView::setGridSnapEnabled(bool enabled) { m_grid_snap_enabled = enabled; }

void PrintView::setGridSnapStep(float step_mm) {
    m_grid_snap_step = std::max(0.001f, step_mm);
}

void PrintView::setModelOpacity(int percent) {
    m_model_overlay_opacity = std::clamp(percent, 0, 100) / 100.0f;
    update();
}

void PrintView::setSupportOpacity(int percent) {
    m_support_overlay_opacity = std::clamp(percent, 0, 100) / 100.0f;
    update();
}

void PrintView::setLayers(const QVector<LayerData> &layers) {
    m_layers = layers;
    m_layer = std::clamp(m_layer, 0, std::max(0, layerCount() - 1));
    if (m_layers.isEmpty())
        m_slice_overlay = false;
    makeSliceOverlay();
    uploadSliceIfReady();
    update();
}

void PrintView::setSlicedSupports(const QVector<float> &vertices) {
    // The post-slice support mesh is a separate tail span. Remove/replace only
    // that span instead of forcing setObjects() to recopy and re-upload a very
    // large model whenever a support setting invalidates the prepared job.
    while (!m_support_ranges.empty() && m_support_ranges.back().object_index < 0) {
        m_support_cpu.resize(m_support_ranges.back().first * 6);
        m_support_ranges.pop_back();
    }
    m_sliced_support_cpu = vertices;
    if (!m_sliced_support_cpu.isEmpty()) {
        m_support_ranges.push_back(
            {int(m_support_cpu.size() / 6), int(m_sliced_support_cpu.size() / 6), -1,
             false, QMatrix4x4()});
        m_support_cpu += m_sliced_support_cpu;
    }
    if (isValid()) {
        makeCurrent();
        uploadBuffer(m_support_vbo, m_support_cpu);
        doneCurrent();
    }
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

void PrintView::setBoxSelectionHandler(BoxSelectionHandler handler) {
    m_box_selection_handler = std::move(handler);
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
    m_model_pick_blocks.clear();
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
                          index, object.selected, object.display_transform};
        m_model_cpu += scene.model_vertices;
        m_model_ranges.push_back(model_range);
        // Keep a small, immutable broad-phase index for selection. A single
        // imported STL may contain millions of triangles; projecting every one
        // on each box-selection release made the GUI appear frozen. Contiguous
        // blocks retain exact final triangle tests while cheaply rejecting the
        // overwhelming majority of the mesh.
        constexpr int vertices_per_pick_block = 256 * 3;
        const int model_end = model_range.first + model_range.count;
        for (int first = model_range.first; first < model_end;
             first += vertices_per_pick_block) {
            const int count = std::min(vertices_per_pick_block, model_end - first);
            QVector3D minimum;
            QVector3D maximum;
            bool initialized = false;
            for (int vertex = first; vertex < first + count; ++vertex) {
                const int offset = vertex * 6;
                const QVector3D point(m_model_cpu[offset], m_model_cpu[offset + 1],
                                      m_model_cpu[offset + 2]);
                if (!initialized) {
                    minimum = maximum = point;
                    initialized = true;
                } else {
                    minimum.setX(std::min(minimum.x(), point.x()));
                    minimum.setY(std::min(minimum.y(), point.y()));
                    minimum.setZ(std::min(minimum.z(), point.z()));
                    maximum.setX(std::max(maximum.x(), point.x()));
                    maximum.setY(std::max(maximum.y(), point.y()));
                    maximum.setZ(std::max(maximum.z(), point.z()));
                }
            }
            if (initialized)
                m_model_pick_blocks.push_back({first, count, index, minimum, maximum,
                                               object.display_transform});
        }
        Range support_range{int(m_support_cpu.size() / 6), int(scene.support_vertices.size() / 6),
                            index, object.selected, object.display_transform};
        m_support_cpu += scene.support_vertices;
        m_support_ranges.push_back(support_range);

        const int artwork_first = m_artwork_cpu.size() / 6;
        if (object.isArtwork()) {
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
        }
        m_artwork_ranges.push_back(
            {artwork_first, int(m_artwork_cpu.size() / 6) - artwork_first, index,
             object.selected, object.display_transform});

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
    if (!m_sliced_support_cpu.isEmpty()) {
        m_support_ranges.push_back(
            {int(m_support_cpu.size() / 6), int(m_sliced_support_cpu.size() / 6), -1,
             false, QMatrix4x4()});
        m_support_cpu += m_sliced_support_cpu;
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

void PrintView::updateObjectTransforms(
    const std::vector<std::unique_ptr<SceneObject>> &objects) {
    const auto update_ranges = [&](std::vector<Range> &ranges) {
        for (Range &range : ranges) {
            if (range.object_index < 0 || range.object_index >= static_cast<int>(objects.size()))
                continue;
            const SceneObject &object = *objects[range.object_index];
            range.selected = object.selected;
            range.transform = object.display_transform;
        }
    };
    update_ranges(m_model_ranges);
    update_ranges(m_support_ranges);
    update_ranges(m_artwork_ranges);
    for (TriangleBlock &block : m_model_pick_blocks) {
        if (block.object_index >= 0 &&
            block.object_index < static_cast<int>(objects.size()))
            block.transform = objects[block.object_index]->display_transform;
    }

    m_pick_bounds.assign(objects.size(), {});
    bool has_bounds = false;
    bool has_selected_bounds = false;
    QVector3D selected_minimum;
    QVector3D selected_maximum;
    for (int index = 0; index < static_cast<int>(objects.size()); ++index) {
        const SceneObject &object = *objects[index];
        if (!object.visible)
            continue;
        const SceneData &scene = object.visual;
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
        if (!object.selected)
            continue;
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
    if (!has_bounds) {
        m_minimum = QVector3D(-1.0f, -1.0f, 0.0f);
        m_maximum = QVector3D(1.0f, 1.0f, 1.0f);
    }
    m_gizmo_center = has_selected_bounds ? (selected_minimum + selected_maximum) * 0.5f
                                          : QVector3D();
    makeGizmo();
    if (isValid()) {
        makeCurrent();
        uploadBuffer(m_gizmo_vbo, m_gizmo_cpu);
        doneCurrent();
    }
    update();
}

void PrintView::resetCamera() {
    const float plate_radius = std::max(m_build_volume.x(), m_build_volume.y()) * 0.58f;
    const float model_radius = std::max(1.0f, (m_maximum - m_minimum).length() * 0.55f);
    m_camera_radius = std::max(plate_radius, model_radius);
    m_camera_centre_z = std::max(0.0f, (m_minimum.z() + m_maximum.z()) * 0.5f);
    m_yaw = -35.0f;
    m_pitch = 32.0f;
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
        uniform mat4 model;
        uniform mat3 normal_matrix;
        out vec3 world_normal;
        void main() {
            gl_Position = mvp * model * vec4(position, 1.0);
            world_normal = normalize(normal_matrix * normal);
        }
    )";
    static const char *fragment_shader = R"(
        #version 330 core
        in vec3 world_normal;
        uniform vec3 base_colour;
        uniform float light_amount;
        uniform float alpha;
        out vec4 fragment;
        void main() {
            vec3 light = normalize(vec3(-0.35, -0.45, 1.0));
            float shaded = 0.35 + 0.65 * max(dot(normalize(world_normal), light), 0.0);
            fragment = vec4(base_colour * mix(1.0, shaded, light_amount), alpha);
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
    m_slice_vbo.create();
    m_slice_fill_vbo.create();
    makePlatform();
    makeGizmo();
    uploadBuffers();
}

void PrintView::resizeGL(int width, int height) { glViewport(0, 0, width, height); }

QMatrix4x4 PrintView::currentMvp() const {
    const float radius = m_camera_radius;
    const QVector3D centre(0.0f, 0.0f, m_camera_centre_z);
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
    const float distance = m_camera_radius * 3.5f / m_zoom;
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

bool PrintView::projectBounds(const QVector3D &minimum, const QVector3D &maximum,
                              const QMatrix4x4 &transform, const QMatrix4x4 &mvp,
                              QRectF &screen_bounds) const {
    bool initialized = false;
    double left = 0.0;
    double right = 0.0;
    double top = 0.0;
    double bottom = 0.0;
    for (int mask = 0; mask < 8; ++mask) {
        const QVector3D local((mask & 1) ? maximum.x() : minimum.x(),
                              (mask & 2) ? maximum.y() : minimum.y(),
                              (mask & 4) ? maximum.z() : minimum.z());
        const QVector3D world = transform.map(local);
        const QVector4D clip = mvp * QVector4D(world, 1.0f);
        // A box crossing the eye/near plane cannot safely be culled using a
        // projected corner rectangle. Let the exact test handle this rare case.
        if (clip.w() <= 1.0e-7f)
            return false;
        const QPointF screen = projectPoint(world, mvp);
        if (!initialized) {
            left = right = screen.x();
            top = bottom = screen.y();
            initialized = true;
        } else {
            left = std::min(left, screen.x());
            right = std::max(right, screen.x());
            top = std::min(top, screen.y());
            bottom = std::max(bottom, screen.y());
        }
    }
    if (initialized)
        screen_bounds = QRectF(QPointF(left, top), QPointF(right, bottom));
    return initialized;
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

bool PrintView::screenRay(const QPointF &position, QVector3D &origin,
                          QVector3D &direction) const {
    bool invertible = false;
    const QMatrix4x4 inverse = currentMvp().inverted(&invertible);
    if (!invertible)
        return false;
    const float x = float(position.x() * 2.0 / std::max(1, width()) - 1.0);
    const float y = float(1.0 - position.y() * 2.0 / std::max(1, height()));
    QVector4D near_point = inverse * QVector4D(x, y, -1.0f, 1.0f);
    QVector4D far_point = inverse * QVector4D(x, y, 1.0f, 1.0f);
    if (std::abs(near_point.w()) < 1.0e-7f || std::abs(far_point.w()) < 1.0e-7f)
        return false;
    near_point /= near_point.w();
    far_point /= far_point.w();
    origin = near_point.toVector3D();
    direction = (far_point - near_point).toVector3D().normalized();
    return !direction.isNull();
}

void PrintView::paintGL() {
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
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (!m_program.isLinked())
        return;

    const QMatrix4x4 mvp = currentMvp();
    m_program.bind();
    m_vao.bind();
    m_program.setUniformValue("mvp", mvp);
    m_program.setUniformValue("normal_matrix", QMatrix3x3());
    m_program.setUniformValue("light_amount", 0.0f);
    m_program.setUniformValue("alpha", 1.0f);
    glDisable(GL_CULL_FACE);
    glLineWidth(1.25f);
    drawRange(m_platform_vbo, 0, m_platform_cpu.size() / 6,
              QVector3D(0.38f, 0.48f, 0.62f), GL_LINES);
    if (m_slice_overlay)
        glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(m_slice_overlay ? GL_FALSE : GL_TRUE);
    for (const Range &range : m_artwork_ranges) {
        drawRange(m_artwork_vbo, range.first, range.count,
                  range.selected ? QVector3D(1.0f, 0.62f, 0.16f)
                                 : QVector3D(0.25f, 0.95f, 0.62f),
                  GL_LINES, m_slice_overlay ? 0.28f : 1.0f, range.transform);
    }

    // Opaque geometry uses the regular depth buffer. A translucent model is
    // resolved as two ordered shells: its nearest back-facing surface first,
    // then its nearest front-facing surface. This exposes an internal contour
    // without accumulating every triangle of a dense mesh into visual noise.
    glDisable(GL_CULL_FACE);
    m_program.setUniformValue("light_amount", 1.0f);
    const auto draw_meshes = [this](QOpenGLBuffer &buffer,
                                    const std::vector<Range> &ranges, float alpha,
                                    const QVector3D &normal_colour,
                                    const QVector3D &selected_colour,
                                    bool show_interior_shell) {
        if (ranges.empty() || alpha <= 0.001f)
            return;
        const auto draw_all = [&](float pass_alpha) {
            for (const Range &range : ranges)
                drawRange(buffer, range.first, range.count,
                          range.selected ? selected_colour : normal_colour,
                          GL_TRIANGLES, pass_alpha, range.transform);
        };
        if (alpha >= 0.999f) {
            glDisable(GL_BLEND);
            glDisable(GL_STENCIL_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_LESS);
            draw_all(1.0f);
            return;
        }

        const auto resolve_nearest_surface = [&](float surface_alpha) {
            // Depth-only pre-pass, followed by one shaded fragment per sample.
            // The stencil rejects duplicate/coincident triangles at that depth.
            glDisable(GL_BLEND);
            glDisable(GL_STENCIL_TEST);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_LESS);
            draw_all(1.0f);

            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_FALSE);
            glDepthFunc(GL_EQUAL);
            glStencilMask(0xff);
            glClearStencil(0);
            glClear(GL_STENCIL_BUFFER_BIT);
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_EQUAL, 0, 0xff);
            glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            draw_all(surface_alpha);
            glDisable(GL_STENCIL_TEST);
        };

        if (show_interior_shell) {
            // Resolve the closest inward-facing wall before the outer shell.
            // Its lower opacity makes cavities/contact penetration readable
            // while avoiding the old grainy all-triangle blend.
            glEnable(GL_CULL_FACE);
            glCullFace(GL_FRONT);
            resolve_nearest_surface(std::min(0.42f, alpha * 0.65f));
            glCullFace(GL_BACK);
            resolve_nearest_surface(alpha);
            glDisable(GL_CULL_FACE);
        } else {
            glDisable(GL_CULL_FACE);
            resolve_nearest_surface(alpha);
        }
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xff);
    };

    const float model_alpha = m_slice_overlay ? m_model_overlay_opacity : 1.0f;
    const float support_alpha = m_slice_overlay ? m_support_overlay_opacity : 1.0f;
    // Supports are composed first. An outer model surface in front of a support
    // then attenuates/tints it; a support in front already owns the nearer depth
    // and remains blue. Clearing the model depth here used to make every support
    // appear on top as soon as model transparency changed from zero.
    draw_meshes(m_support_vbo, m_support_ranges, support_alpha,
                QVector3D(0.12f, 0.42f, 0.95f), QVector3D(0.95f, 0.55f, 0.12f),
                false);
    draw_meshes(m_model_vbo, m_model_ranges, model_alpha,
                QVector3D(0.18f, 0.76f, 0.73f), QVector3D(1.0f, 0.62f, 0.16f),
                m_slice_overlay && model_alpha < 0.999f);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glDisable(GL_STENCIL_TEST);
    if (m_slice_overlay) {
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        m_program.setUniformValue("light_amount", 0.0f);
        if (!m_slice_fill_ranges.empty()) {
            // Build a non-zero-winding mask. Outer contours increment and hole
            // contours decrement, while overlapping support/model solids stay
            // filled instead of XOR-ing into artificial empty regions.
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0xff);
            glClearStencil(0);
            glClear(GL_STENCIL_BUFFER_BIT);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glStencilFunc(GL_ALWAYS, 0, 0xff);
            glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
            glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
            for (const DrawSpan &span : m_slice_fill_ranges)
                drawRange(m_slice_fill_vbo, span.first, span.count,
                          QVector3D(0.20f, 1.0f, 0.92f), GL_TRIANGLE_FAN);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glStencilFunc(GL_NOTEQUAL, 0, 0xff);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            drawRange(m_slice_fill_vbo, m_slice_fill_quad_first, 6,
                      QVector3D(0.20f, 1.0f, 0.92f), GL_TRIANGLES, 0.5f);
            glDisable(GL_STENCIL_TEST);
            glStencilMask(0xff);
        }
        glEnable(GL_LINE_SMOOTH);
        glLineWidth(2.0f);
        drawRange(m_slice_vbo, 0, m_slice_cpu.size() / 6,
                  QVector3D(0.20f, 1.0f, 0.92f), GL_LINES, 1.0f);
        glDisable(GL_LINE_SMOOTH);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }
    glDisable(GL_CULL_FACE);
    glLineWidth(3.0f);
    if (!m_slice_overlay) {
        drawRange(m_gizmo_vbo, 0, 2, QVector3D(0.95f, 0.2f, 0.2f), GL_LINES);
        drawRange(m_gizmo_vbo, 2, 2, QVector3D(0.2f, 0.9f, 0.3f), GL_LINES);
        drawRange(m_gizmo_vbo, 4, 2, QVector3D(0.25f, 0.45f, 1.0f), GL_LINES);
    }
    m_vao.release();
    m_program.release();
    if (m_box_selecting) {
        QPainter painter(this);
        const bool crossing = m_selection_rect.width() >= 0.0;
        const QRectF rectangle = m_selection_rect.normalized();
        const QColor colour = crossing ? QColor(55, 185, 255) : QColor(92, 230, 135);
        painter.setPen(QPen(colour, 1.0));
        painter.setBrush(QColor(colour.red(), colour.green(), colour.blue(), 38));
        painter.drawRect(rectangle);
    }
}

namespace {

QVector3D vertexAt(const QVector<float> &vertices, int index) {
    const int offset = index * 6;
    return QVector3D(vertices[offset], vertices[offset + 1], vertices[offset + 2]);
}

QVector3D transformedVertexAt(const QVector<float> &vertices, int index,
                              const QMatrix4x4 &transform) {
    return transform.map(vertexAt(vertices, index));
}

bool rayTriangle(const QVector3D &origin, const QVector3D &direction, const QVector3D &a,
                 const QVector3D &b, const QVector3D &c, float &distance) {
    const QVector3D edge1 = b - a;
    const QVector3D edge2 = c - a;
    const QVector3D p = QVector3D::crossProduct(direction, edge2);
    const float determinant = QVector3D::dotProduct(edge1, p);
    if (std::abs(determinant) < 1.0e-8f)
        return false;
    const float inverse = 1.0f / determinant;
    const QVector3D t = origin - a;
    const float u = QVector3D::dotProduct(t, p) * inverse;
    if (u < 0.0f || u > 1.0f)
        return false;
    const QVector3D q = QVector3D::crossProduct(t, edge1);
    const float v = QVector3D::dotProduct(direction, q) * inverse;
    if (v < 0.0f || u + v > 1.0f)
        return false;
    distance = QVector3D::dotProduct(edge2, q) * inverse;
    return distance >= 0.0f;
}

float pointSegmentDistance(const QPointF &point, const QPointF &a, const QPointF &b) {
    const QPointF segment = b - a;
    const double length_squared = segment.x() * segment.x() + segment.y() * segment.y();
    if (length_squared <= 1.0e-12)
        return float(QLineF(point, a).length());
    const QPointF relative = point - a;
    const double ratio = std::clamp((relative.x() * segment.x() + relative.y() * segment.y()) /
                                        length_squared,
                                    0.0, 1.0);
    return float(QLineF(point, a + segment * ratio).length());
}

bool pointInTriangle(const QPointF &point, const std::array<QPointF, 3> &triangle) {
    const auto sign = [](const QPointF &p1, const QPointF &p2, const QPointF &p3) {
        return (p1.x() - p3.x()) * (p2.y() - p3.y()) -
               (p2.x() - p3.x()) * (p1.y() - p3.y());
    };
    const double first = sign(point, triangle[0], triangle[1]);
    const double second = sign(point, triangle[1], triangle[2]);
    const double third = sign(point, triangle[2], triangle[0]);
    const bool negative = first < 0.0 || second < 0.0 || third < 0.0;
    const bool positive = first > 0.0 || second > 0.0 || third > 0.0;
    return !(negative && positive);
}

bool segmentIntersectsRect(const QPointF &a, const QPointF &b, const QRectF &rect) {
    if (rect.contains(a) || rect.contains(b))
        return true;
    const std::array<QLineF, 4> edges{
        QLineF(rect.topLeft(), rect.topRight()), QLineF(rect.topRight(), rect.bottomRight()),
        QLineF(rect.bottomRight(), rect.bottomLeft()), QLineF(rect.bottomLeft(), rect.topLeft())};
    const QLineF segment(a, b);
    return std::any_of(edges.begin(), edges.end(), [&](const QLineF &edge) {
        QPointF intersection;
        return segment.intersects(edge, &intersection) == QLineF::BoundedIntersection;
    });
}

bool triangleIntersectsRect(const std::array<QPointF, 3> &triangle, const QRectF &rect) {
    if (std::any_of(triangle.begin(), triangle.end(),
                    [&](const QPointF &point) { return rect.contains(point); }))
        return true;
    const std::array<QPointF, 4> corners{rect.topLeft(), rect.topRight(), rect.bottomRight(),
                                        rect.bottomLeft()};
    if (std::any_of(corners.begin(), corners.end(),
                    [&](const QPointF &point) { return pointInTriangle(point, triangle); }))
        return true;
    for (int index = 0; index < 3; ++index)
        if (segmentIntersectsRect(triangle[index], triangle[(index + 1) % 3], rect))
            return true;
    return false;
}

} // namespace

int PrintView::pickObject(const QPointF &position) const {
    QVector3D origin;
    QVector3D direction;
    if (!screenRay(position, origin, direction))
        return -1;
    int best = -1;
    float best_distance = std::numeric_limits<float>::max();
    const QMatrix4x4 mvp = currentMvp();
    const auto test_triangles = [&](const QVector<float> &vertices,
                                    const std::vector<TriangleBlock> &blocks) {
        for (const TriangleBlock &block : blocks) {
            QRectF screen_bounds;
            if (projectBounds(block.minimum, block.maximum, block.transform, mvp,
                              screen_bounds) &&
                !screen_bounds.adjusted(-1.0, -1.0, 1.0, 1.0).contains(position))
                continue;
            for (int vertex = block.first; vertex + 2 < block.first + block.count;
                 vertex += 3) {
                float distance = 0.0f;
                if (rayTriangle(origin, direction,
                                transformedVertexAt(vertices, vertex, block.transform),
                                transformedVertexAt(vertices, vertex + 1, block.transform),
                                transformedVertexAt(vertices, vertex + 2, block.transform),
                                distance) &&
                    distance < best_distance) {
                    best_distance = distance;
                    best = block.object_index;
                }
            }
        }
    };
    test_triangles(m_model_cpu, m_model_pick_blocks);
    if (best >= 0)
        return best;

    float best_line_distance = 6.0f;
    float best_depth = std::numeric_limits<float>::max();
    for (const Range &range : m_artwork_ranges) {
        int crossings = 0;
        float range_depth = std::numeric_limits<float>::max();
        for (int vertex = range.first; vertex + 1 < range.first + range.count; vertex += 2) {
            const QVector3D first_world =
                transformedVertexAt(m_artwork_cpu, vertex, range.transform);
            const QVector3D second_world =
                transformedVertexAt(m_artwork_cpu, vertex + 1, range.transform);
            const QVector4D first_clip = mvp * QVector4D(first_world, 1.0f);
            const QVector4D second_clip = mvp * QVector4D(second_world, 1.0f);
            if (first_clip.w() <= 0.0f || second_clip.w() <= 0.0f)
                continue;
            const float distance = pointSegmentDistance(
                position, projectPoint(first_world, mvp), projectPoint(second_world, mvp));
            const float depth = (first_clip.z() / first_clip.w() + second_clip.z() / second_clip.w()) *
                                0.5f;
            range_depth = std::min(range_depth, depth);
            const QPointF a = projectPoint(first_world, mvp);
            const QPointF b = projectPoint(second_world, mvp);
            if ((a.y() > position.y()) != (b.y() > position.y())) {
                const double crossing_x =
                    a.x() + (b.x() - a.x()) * (position.y() - a.y()) / (b.y() - a.y());
                if (crossing_x > position.x())
                    ++crossings;
            }
            if (distance <= best_line_distance && depth < best_depth) {
                best_line_distance = distance;
                best_depth = depth;
                best = range.object_index;
            }
        }
        if ((crossings & 1) != 0 && range_depth < best_depth) {
            best_depth = range_depth;
            best = range.object_index;
        }
    }
    return best;
}

std::vector<int> PrintView::objectsInScreenRect(const QRectF &rect, bool crossing) const {
    const QRectF normalized = rect.normalized();
    const QMatrix4x4 mvp = currentMvp();
    std::vector<bool> found(m_pick_bounds.size(), false);
    std::vector<bool> inside(m_pick_bounds.size(), true);
    std::vector<bool> touched(m_pick_bounds.size(), false);
    const auto projected = [&](const QVector3D &point, QPointF &screen) {
        const QVector4D clip = mvp * QVector4D(point, 1.0f);
        if (clip.w() <= 0.0f)
            return false;
        screen = projectPoint(point, mvp);
        return true;
    };
    const auto overlaps = [](const QRectF &left, const QRectF &right) {
        return left.right() >= right.left() && left.left() <= right.right() &&
               left.bottom() >= right.top() && left.top() <= right.bottom();
    };
    const auto contains_rect = [](const QRectF &outer, const QRectF &inner) {
        return outer.contains(inner.topLeft()) && outer.contains(inner.bottomRight());
    };
    const auto test_triangles = [&](const QVector<float> &vertices,
                                    const std::vector<TriangleBlock> &blocks) {
        for (const TriangleBlock &block : blocks) {
            const int object_index = block.object_index;
            found[object_index] = true;
            if ((crossing && touched[object_index]) || (!crossing && !inside[object_index]))
                continue;
            QRectF block_bounds;
            const bool bounded = projectBounds(block.minimum, block.maximum, block.transform,
                                               mvp, block_bounds);
            if (bounded && crossing && !overlaps(block_bounds, normalized))
                continue;
            if (bounded && contains_rect(normalized, block_bounds)) {
                if (crossing)
                    touched[object_index] = true;
                continue;
            }
            for (int vertex = block.first; vertex + 2 < block.first + block.count;
                 vertex += 3) {
                std::array<QPointF, 3> triangle;
                bool visible = true;
                for (int corner = 0; corner < 3; ++corner)
                    visible = projected(transformedVertexAt(vertices, vertex + corner,
                                                            block.transform),
                                        triangle[corner]) &&
                              visible;
                if (!visible) {
                    inside[object_index] = false;
                    continue;
                }
                if (crossing && triangleIntersectsRect(triangle, normalized))
                    touched[object_index] = true;
                if (!crossing &&
                    !std::all_of(triangle.begin(), triangle.end(), [&](const QPointF &point) {
                        return normalized.contains(point);
                    }))
                    inside[object_index] = false;
                if ((crossing && touched[object_index]) ||
                    (!crossing && !inside[object_index]))
                    break;
            }
        }
    };
    test_triangles(m_model_cpu, m_model_pick_blocks);
    for (const Range &range : m_artwork_ranges) {
        const auto contains_screen_point = [&](const QPointF &point) {
            int crossings = 0;
            for (int vertex = range.first; vertex + 1 < range.first + range.count; vertex += 2) {
                QPointF a;
                QPointF b;
                if (!projected(transformedVertexAt(m_artwork_cpu, vertex, range.transform), a) ||
                    !projected(transformedVertexAt(m_artwork_cpu, vertex + 1,
                                                   range.transform),
                               b))
                    continue;
                if ((a.y() > point.y()) != (b.y() > point.y())) {
                    const double crossing_x =
                        a.x() + (b.x() - a.x()) * (point.y() - a.y()) / (b.y() - a.y());
                    if (crossing_x > point.x())
                        ++crossings;
                }
            }
            return (crossings & 1) != 0;
        };
        for (int vertex = range.first; vertex + 1 < range.first + range.count; vertex += 2) {
            QPointF a;
            QPointF b;
            const bool visible =
                projected(transformedVertexAt(m_artwork_cpu, vertex, range.transform), a) &&
                projected(transformedVertexAt(m_artwork_cpu, vertex + 1, range.transform), b);
            found[range.object_index] = true;
            if (!visible) {
                inside[range.object_index] = false;
                continue;
            }
            if (crossing && segmentIntersectsRect(a, b, normalized))
                touched[range.object_index] = true;
            if (!crossing && (!normalized.contains(a) || !normalized.contains(b)))
                inside[range.object_index] = false;
        }
        if (crossing && !touched[range.object_index]) {
            const std::array<QPointF, 4> corners{normalized.topLeft(), normalized.topRight(),
                                                normalized.bottomRight(),
                                                normalized.bottomLeft()};
            touched[range.object_index] =
                std::any_of(corners.begin(), corners.end(), contains_screen_point);
        }
    }
    std::vector<int> result;
    for (int index = 0; index < static_cast<int>(found.size()); ++index)
        if (found[index] && (crossing ? touched[index] : inside[index]))
            result.push_back(index);
    return result;
}

void PrintView::mousePressEvent(QMouseEvent *event) {
    m_last_mouse = m_press_mouse = event->position();
    m_moved = false;
    const bool orbit = event->button() == Qt::MiddleButton;
    m_pressed_object = event->button() == Qt::LeftButton && !orbit
                           ? pickObject(event->position())
                           : -1;
    if (event->button() == Qt::LeftButton && m_selection_handler)
        if (m_pressed_object >= 0)
            m_selection_handler(m_pressed_object, event->modifiers());
    if (event->button() == Qt::LeftButton && m_pressed_object < 0 && !orbit &&
        !m_slice_overlay) {
        m_box_selecting = true;
        m_selection_rect = QRectF(m_press_mouse, m_press_mouse);
        m_selection_modifiers = event->modifiers();
        update();
    }
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
    if (m_box_selecting && (event->buttons() & Qt::LeftButton)) {
        m_selection_rect = QRectF(m_press_mouse, event->position());
        update();
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
            QVector3D snapped = total;
            if (m_grid_snap_enabled) {
                snapped.setX(std::round(total.x() / m_grid_snap_step) * m_grid_snap_step);
                snapped.setY(std::round(total.y() / m_grid_snap_step) * m_grid_snap_step);
            }
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
        const bool pan = (event->buttons() & Qt::RightButton) ||
                         ((event->buttons() & Qt::LeftButton) &&
                          (event->modifiers() & Qt::ShiftModifier));
        if (pan) {
            const float scale = worldUnitsPerPixel();
            m_pan += QVector2D(float(delta.x()) * scale, float(-delta.y()) * scale);
            update();
        } else if (event->buttons() & Qt::MiddleButton) {
            m_yaw -= float(delta.x()) * 0.5f;
            m_pitch = std::clamp(m_pitch + float(delta.y()) * 0.5f, -89.9f, 89.9f);
            update();
        }
    }
    m_last_mouse = event->position();
}

void PrintView::mouseReleaseEvent(QMouseEvent *event) {
    const bool finished_transform = m_dragging_object;
    if (m_box_selecting && event->button() == Qt::LeftButton) {
        const bool crossing = event->position().x() >= m_press_mouse.x();
        const QRectF rectangle(m_press_mouse, event->position());
        // Remove the overlay before selection callbacks rebuild side panels or
        // repaint object state. With very large meshes the blue rectangle must
        // never remain visually stuck while the result is being committed.
        m_box_selecting = false;
        update();
        if ((event->position() - m_press_mouse).manhattanLength() <= 3.0) {
            if (m_selection_handler)
                m_selection_handler(-1, m_selection_modifiers);
        } else if (m_box_selection_handler) {
            m_box_selection_handler(objectsInScreenRect(rectangle, crossing),
                                    m_selection_modifiers);
        }
    }
    m_dragging_object = false;
    m_pressed_object = -1;
    if (finished_transform && m_transform_finished_handler)
        m_transform_finished_handler();
}

void PrintView::wheelEvent(QWheelEvent *event) {
    const float previous_units = worldUnitsPerPixel();
    const float previous_zoom = m_zoom;
    m_zoom = std::clamp(previous_zoom * std::exp(float(event->angleDelta().y()) / 900.0f),
                        0.05f, 250.0f);
    if (m_zoom != previous_zoom) {
        const float difference = previous_units - worldUnitsPerPixel();
        const float dx = float(event->position().x() - width() * 0.5);
        const float dy = float(event->position().y() - height() * 0.5);
        m_pan.setX(m_pan.x() - dx * difference);
        m_pan.setY(m_pan.y() + dy * difference);
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

void PrintView::uploadSliceIfReady() {
    if (!isValid())
        return;
    makeCurrent();
    uploadBuffer(m_slice_vbo, m_slice_cpu);
    uploadBuffer(m_slice_fill_vbo, m_slice_fill_cpu);
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
    uploadBuffer(m_slice_vbo, m_slice_cpu);
    uploadBuffer(m_slice_fill_vbo, m_slice_fill_cpu);
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

void PrintView::makeSliceOverlay() {
    m_slice_cpu.clear();
    m_slice_fill_cpu.clear();
    m_slice_fill_ranges.clear();
    m_slice_fill_quad_first = 0;
    if (m_layer < 0 || m_layer >= m_layers.size())
        return;
    const LayerData &layer = m_layers[m_layer];
    const float z = layer.z + 0.001f;
    QVector2D minimum(std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max());
    QVector2D maximum(-std::numeric_limits<float>::max(),
                      -std::numeric_limits<float>::max());
    for (const QVector<QVector2D> &contour : layer.contours) {
        if (contour.size() < 2)
            continue;
        for (qsizetype index = 0; index < contour.size(); ++index) {
            const QVector2D first = contour[index];
            const QVector2D second = contour[(index + 1) % contour.size()];
            appendLine(m_slice_cpu, QVector3D(first.x(), first.y(), z),
                       QVector3D(second.x(), second.y(), z));
            minimum.setX(std::min(minimum.x(), first.x()));
            minimum.setY(std::min(minimum.y(), first.y()));
            maximum.setX(std::max(maximum.x(), first.x()));
            maximum.setY(std::max(maximum.y(), first.y()));
        }
        if (contour.size() >= 3) {
            const int first = m_slice_fill_cpu.size() / 6;
            for (const QVector2D &point : contour)
                appendLineVertex(m_slice_fill_cpu, QVector3D(point.x(), point.y(), z));
            m_slice_fill_ranges.push_back({first, int(contour.size())});
        }
    }
    if (m_slice_fill_ranges.empty())
        return;
    m_slice_fill_quad_first = m_slice_fill_cpu.size() / 6;
    const QVector3D bottom_left(minimum.x(), minimum.y(), z);
    const QVector3D bottom_right(maximum.x(), minimum.y(), z);
    const QVector3D top_right(maximum.x(), maximum.y(), z);
    const QVector3D top_left(minimum.x(), maximum.y(), z);
    for (const QVector3D &point : {bottom_left, bottom_right, top_right,
                                   bottom_left, top_right, top_left})
        appendLineVertex(m_slice_fill_cpu, point);
}

void PrintView::drawRange(QOpenGLBuffer &buffer, int first, int count,
                          const QVector3D &colour, GLenum primitive, float alpha,
                          const QMatrix4x4 &model) {
    if (!buffer.isCreated() || count == 0)
        return;
    buffer.bind();
    m_program.enableAttributeArray(0);
    m_program.enableAttributeArray(1);
    m_program.setAttributeBuffer(0, GL_FLOAT, 0, 3, 6 * int(sizeof(float)));
    m_program.setAttributeBuffer(1, GL_FLOAT, 3 * int(sizeof(float)), 3,
                                 6 * int(sizeof(float)));
    m_program.setUniformValue("model", model);
    m_program.setUniformValue("normal_matrix", model.normalMatrix());
    m_program.setUniformValue("base_colour", colour);
    m_program.setUniformValue("alpha", alpha);
    glDrawArrays(primitive, first, count);
    buffer.release();
}
