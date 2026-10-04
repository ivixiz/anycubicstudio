#pragma once

#include "scene.hpp"

#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPointF>
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
    using TransformHandler = std::function<void(GizmoMode, QVector3D)>;

    explicit PrintView(QWidget *parent = nullptr);
    ~PrintView() override;

    int layerCount() const;
    float layerZ(int index) const;
    void setLayerMode(bool enabled);
    void setLayer(int index);
    void setShowModel(bool enabled);
    void setShowSupports(bool enabled);
    void setBuildVolume(const QVector3D &volume);
    void setLayers(const QVector<LayerData> &layers);
    void setObjects(const std::vector<std::unique_ptr<SceneObject>> &objects);
    void setGizmoMode(GizmoMode mode);
    void setSelectionHandler(SelectionHandler handler);
    void setTransformHandler(TransformHandler handler);
    void resetCamera();
    void fitLayer();

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
    };
    struct PickBounds {
        QVector3D minimum;
        QVector3D maximum;
        bool valid = false;
        bool selected = false;
    };

    void uploadIfReady();
    void uploadBuffer(QOpenGLBuffer &buffer, const QVector<float> &values);
    void uploadBuffers();
    void appendLineVertex(QVector<float> &target, const QVector3D &point);
    void appendLine(QVector<float> &target, const QVector3D &first, const QVector3D &second);
    void makePlatform();
    void makeGizmo();
    void drawRange(QOpenGLBuffer &buffer, int first, int count, const QVector3D &colour,
                   GLenum primitive);
    void drawLayer();
    QMatrix4x4 currentMvp() const;
    int pickObject(const QPointF &position) const;

    QVector<float> m_model_cpu;
    QVector<float> m_support_cpu;
    QVector<float> m_artwork_cpu;
    QVector<float> m_platform_cpu;
    QVector<float> m_gizmo_cpu;
    std::vector<Range> m_model_ranges;
    std::vector<Range> m_support_ranges;
    std::vector<Range> m_artwork_ranges;
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
    QPointF m_last_mouse;
    QPointF m_press_mouse;
    QVector2D m_pan;
    QVector2D m_layer_pan;
    float m_yaw = -35.0f;
    float m_pitch = 58.0f;
    float m_zoom = 1.0f;
    float m_layer_zoom = 1.0f;
    bool m_layer_mode = false;
    bool m_show_model = true;
    bool m_show_supports = true;
    bool m_dragging_object = false;
    bool m_moved = false;
    int m_pressed_object = -1;
    int m_layer = 0;
    GizmoMode m_gizmo_mode = GizmoMode::Move;
    SelectionHandler m_selection_handler;
    TransformHandler m_transform_handler;
};
