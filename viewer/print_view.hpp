#pragma once

#include "scene.hpp"

#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPointF>
#include <QRectF>
#include <QVector2D>

#include <functional>
#include <memory>
#include <vector>

class QMouseEvent;
class QWheelEvent;

class PrintView final : public QOpenGLWidget, protected QOpenGLFunctions {
public:
    enum class GizmoMode { Move, Rotate, Scale };
    using SelectionHandler = std::function<void(int, Qt::KeyboardModifiers)>;
    using BoxSelectionHandler =
        std::function<void(const std::vector<int> &, Qt::KeyboardModifiers)>;
    using TransformHandler = std::function<void(GizmoMode, QVector3D)>;
    using TransformFinishedHandler = std::function<void()>;

    explicit PrintView(QWidget *parent = nullptr);
    ~PrintView() override;

    int layerCount() const;
    float layerZ(int index) const;
    bool sliceOverlayEnabled() const { return m_slice_overlay; }
    void setSliceOverlay(bool enabled);
    void setLayer(int index);
    void setBuildVolume(const QVector3D &volume);
    void setGridSnapEnabled(bool enabled);
    void setGridSnapStep(float step_mm);
    void setModelOpacity(int percent);
    void setSupportOpacity(int percent);
    void setLayers(const QVector<LayerData> &layers);
    void setSlicedSupports(const QVector<float> &vertices);
    void setObjects(const std::vector<std::unique_ptr<SceneObject>> &objects);
    void updateObjectTransforms(const std::vector<std::unique_ptr<SceneObject>> &objects);
    void setGizmoMode(GizmoMode mode);
    void setRotationZOnly(bool enabled);
    void setMinimumSelectedScale(float scale);
    void setSelectionHandler(SelectionHandler handler);
    void setBoxSelectionHandler(BoxSelectionHandler handler);
    void setTransformHandler(TransformHandler handler);
    void setTransformFinishedHandler(TransformFinishedHandler handler);
    void resetCamera();

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    struct Range {
        int first = 0;
        int count = 0;
        int object_index = -1;
        bool selected = false;
        QMatrix4x4 transform;
    };
    struct PickBounds {
        QVector3D minimum;
        QVector3D maximum;
        bool valid = false;
        bool selected = false;
    };
    struct TriangleBlock {
        int first = 0;
        int count = 0;
        int object_index = -1;
        QVector3D minimum;
        QVector3D maximum;
        QMatrix4x4 transform;
    };
    struct DrawSpan {
        int first = 0;
        int count = 0;
    };

    void uploadIfReady();
    void uploadSliceIfReady();
    void uploadBuffer(QOpenGLBuffer &buffer, const QVector<float> &values);
    void uploadBuffers();
    void appendLineVertex(QVector<float> &target, const QVector3D &point);
    void appendLine(QVector<float> &target, const QVector3D &first, const QVector3D &second);
    void makePlatform();
    void makeGizmo();
    void makeSliceOverlay();
    void drawRange(QOpenGLBuffer &buffer, int first, int count, const QVector3D &colour,
                   GLenum primitive, float alpha = 1.0f,
                   const QMatrix4x4 &model = QMatrix4x4());
    QMatrix4x4 currentMvp() const;
    QVector3D cameraForward() const;
    float worldUnitsPerPixel() const;
    QPointF projectPoint(const QVector3D &point, const QMatrix4x4 &mvp) const;
    bool projectBounds(const QVector3D &minimum, const QVector3D &maximum,
                       const QMatrix4x4 &transform, const QMatrix4x4 &mvp,
                       QRectF &screen_bounds) const;
    bool unprojectToDragPlane(const QPointF &position, QVector3D &point) const;
    bool screenRay(const QPointF &position, QVector3D &origin, QVector3D &direction) const;
    int pickObject(const QPointF &position) const;
    std::vector<int> objectsInScreenRect(const QRectF &rect, bool crossing) const;

    QVector<float> m_model_cpu;
    QVector<float> m_support_cpu;
    QVector<float> m_sliced_support_cpu;
    QVector<float> m_artwork_cpu;
    QVector<float> m_platform_cpu;
    QVector<float> m_gizmo_cpu;
    QVector<float> m_slice_cpu;
    QVector<float> m_slice_fill_cpu;
    std::vector<Range> m_model_ranges;
    std::vector<Range> m_support_ranges;
    std::vector<Range> m_artwork_ranges;
    std::vector<TriangleBlock> m_model_pick_blocks;
    std::vector<DrawSpan> m_slice_fill_ranges;
    std::vector<PickBounds> m_pick_bounds;
    QVector<LayerData> m_layers;
    QVector3D m_build_volume{M7_BUILD_X, M7_BUILD_Y, M7_BUILD_Z};
    QVector3D m_minimum{-1.0f, -1.0f, 0.0f};
    QVector3D m_maximum{1.0f, 1.0f, 1.0f};
    QVector3D m_gizmo_center;
    QOpenGLShaderProgram m_program;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer m_model_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_support_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_artwork_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_platform_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_gizmo_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_slice_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_slice_fill_vbo{QOpenGLBuffer::VertexBuffer};
    QPointF m_last_mouse;
    QPointF m_press_mouse;
    QPointF m_gizmo_screen_center;
    QVector2D m_pan;
    float m_yaw = -35.0f;
    float m_pitch = 32.0f;
    float m_zoom = 1.0f;
    float m_camera_radius = 130.0f;
    float m_camera_centre_z = 0.0f;
    float m_grid_snap_step = 1.0f;
    float m_model_overlay_opacity = 0.22f;
    float m_support_overlay_opacity = 0.22f;
    bool m_grid_snap_enabled = true;
    bool m_slice_overlay = false;
    bool m_box_selecting = false;
    bool m_dragging_object = false;
    bool m_drag_unproject_valid = false;
    bool m_rotation_z_only = false;
    bool m_moved = false;
    int m_pressed_object = -1;
    int m_layer = 0;
    int m_slice_fill_quad_first = 0;
    QRectF m_selection_rect;
    Qt::KeyboardModifiers m_selection_modifiers{};
    QMatrix4x4 m_drag_inverse_mvp;
    QVector3D m_drag_plane_point;
    QVector3D m_drag_plane_normal;
    QVector3D m_drag_start_world;
    QVector3D m_drag_emitted_move;
    QVector3D m_drag_rotation_axis{0.0f, 0.0f, 1.0f};
    float m_drag_last_angle = 0.0f;
    float m_drag_angle_total = 0.0f;
    float m_drag_emitted_angle = 0.0f;
    float m_drag_start_radius = 0.0f;
    int m_drag_emitted_scale_steps = 0;
    int m_minimum_selected_scale = 1;
    GizmoMode m_gizmo_mode = GizmoMode::Move;
    SelectionHandler m_selection_handler;
    BoxSelectionHandler m_box_selection_handler;
    TransformHandler m_transform_handler;
    TransformFinishedHandler m_transform_finished_handler;
};
