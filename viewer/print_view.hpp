#pragma once

#include "scene.hpp"

#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPointF>
#include <QVector2D>

class QMouseEvent;
class QWheelEvent;

class PrintView final : public QOpenGLWidget, protected QOpenGLFunctions {
public:
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
    void setModelScenes(const std::vector<std::unique_ptr<ModelState>> &models, int selected);
    void resetCamera();

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    struct Range {
        int first = 0;
        int count = 0;
        bool selected = false;
    };

    void uploadIfReady();
    void uploadBuffer(QOpenGLBuffer &buffer, const QVector<float> &values);
    void uploadBuffers();
    void appendPlatformVertex(const QVector3D &point);
    void appendPlatformEdge(const QVector3D &first, const QVector3D &second);
    void makePlatform();
    void drawRange(QOpenGLBuffer &buffer, int first, int count, const QVector3D &colour,
                   GLenum primitive);
    void drawLayer();

    QVector<float> m_model_cpu;
    QVector<float> m_support_cpu;
    QVector<float> m_platform_cpu;
    std::vector<Range> m_model_ranges;
    std::vector<Range> m_support_ranges;
    QVector<LayerData> m_layers;
    QVector3D m_build_volume{M7_BUILD_X, M7_BUILD_Y, M7_BUILD_Z};
    QVector3D m_minimum{-1.0f, -1.0f, 0.0f};
    QVector3D m_maximum{1.0f, 1.0f, 1.0f};
    QOpenGLShaderProgram m_program;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer m_model_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_support_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_platform_vbo{QOpenGLBuffer::VertexBuffer};
    QPointF m_last_mouse;
    QVector2D m_pan;
    float m_yaw = -35.0f;
    float m_pitch = 58.0f;
    float m_zoom = 1.0f;
    bool m_layer_mode = false;
    bool m_show_model = true;
    bool m_show_supports = true;
    int m_layer = 0;
};
