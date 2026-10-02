#include "main_window.hpp"

#include <QApplication>
#include <QCoreApplication>
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
    for (int index = 1; index < arguments.size(); ++index)
        initial_files.append(QFileInfo(arguments[index]).absoluteFilePath());
    MainWindow window(initial_files);
    window.show();
    return application.exec();
}
