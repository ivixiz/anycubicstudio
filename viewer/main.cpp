#include "main_window.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QOpenGLContext>
#include <QSurfaceFormat>

int main(int argc, char *argv[]) {
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication application(argc, argv);
    QCoreApplication::setApplicationName("Anycubic Studio");
    QCoreApplication::setOrganizationName("AnycubicStudioCommunity");
    QStringList initial_files;
    const QStringList arguments = application.arguments();
    bool smoke_import = false;
    bool smoke_history = false;
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
        return 0;
    }
    window.show();
    return application.exec();
}
