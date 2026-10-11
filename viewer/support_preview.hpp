#pragma once

#include "scene.hpp"

#include <QWidget>

class SupportPreview final : public QWidget {
public:
    explicit SupportPreview(QWidget *parent = nullptr);

    void setSettings(const SupportSettings &settings);
    void setFocusedParameter(const QString &parameter);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    SupportSettings m_settings;
    QString m_focused_parameter;
};
