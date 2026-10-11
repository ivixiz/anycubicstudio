#include "main_window.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QOpenGLContext>
#include <QPixmap>
#include <QSurfaceFormat>

int main(int argc, char *argv[]) {
    Q_INIT_RESOURCE(icons);

    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication application(argc, argv);
    QCoreApplication::setApplicationName("Anycubic Studio");
    QCoreApplication::setOrganizationName("AnycubicStudioCommunity");
    const QStringList required_icons = {
        ":/icons/abort.png",  ":/icons/redo.png",   ":/icons/undo.png",
        ":/icons/scale.png",  ":/icons/move.png",   ":/icons/pause.png",
        ":/icons/status.png", ":/icons/resume.png", ":/icons/run.png",
        ":/icons/slice.png",  ":/icons/fit.png",    ":/icons/import.png",
        ":/icons/rotate.png", ":/icons/layout.png",
    };
    for (const QString &path : required_icons) {
        if (QPixmap(path).isNull()) {
            qCritical().noquote() << "Embedded toolbar icon is missing or invalid:" << path;
            return 4;
        }
    }
    QStringList initial_files;
    const QStringList arguments = application.arguments();
    bool smoke_import = false;
    bool smoke_history = false;
    bool smoke_support_preview = false;
    for (int index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == "--smoke-import") {
            smoke_import = true;
            continue;
        }
        if (arguments[index] == "--smoke-history") {
            smoke_import = true;
            smoke_history = true;
            continue;
        }
        if (arguments[index] == "--smoke-support-preview") {
            smoke_import = true;
            smoke_support_preview = true;
            continue;
        }
        initial_files.append(QFileInfo(arguments[index]).absoluteFilePath());
    }
    MainWindow window(initial_files, nullptr, smoke_import);
    if (smoke_import) {
        const auto expected = static_cast<std::size_t>(initial_files.size());
        if (window.importedObjectCount() != expected) {
            qCritical().noquote()
                << QString("Import smoke test failed: imported %1 of %2 object(s).")
                       .arg(window.importedObjectCount())
                       .arg(expected);
            return 2;
        }
        qInfo().noquote()
            << QString("Import smoke test passed: %1 object(s).").arg(expected);
        if (smoke_history) {
            QString error;
            if (!window.runHistorySmokeTest(error)) {
                qCritical().noquote() << "History smoke test failed:" << error;
                return 3;
            }
            qInfo().noquote() << "History smoke test passed: transform and setting undo/redo.";
        }
        if (smoke_support_preview) {
            QString error;
            if (!window.runSupportPreviewSmokeTest(error)) {
                qCritical().noquote() << "Support preview smoke test failed:" << error;
                return 5;
            }
            qInfo().noquote()
                << "Support preview smoke test passed: show, live redraw, and hide.";
        }
        return 0;
    }
    window.show();
    return application.exec();
}
