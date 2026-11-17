#include "mainwindow.h"
#include "mainwindow.cpp"

#include <QApplication>
#include <QCoreApplication>

QString getExePath() {
    QString appPath;

#if defined(Q_OS_WIN)
    char path[MAX_PATH];
    HMODULE hModule = GetModuleHandle(nullptr);
    if (hModule != nullptr) {
        GetModuleFileNameA(hModule, path, MAX_PATH);
        appPath = QString::fromLocal8Bit(path);
    }
#else
    appPath = QFileInfo(QDir::currentPath()).absolutePath();
#endif

    return QFileInfo(appPath).dir().path();
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    //QString appDir = getExePath(); // Получаем путь к .exe
    // qDebug() << "Путь к приложению:" << appDir;

    // if (appDir.isEmpty()) {
    //     qDebug() << "Ошибка: Не удалось получить путь к приложению";
    //     return -1;
    // }

    // QString pluginPath = appDir + "/plugins/gstreamer-1.0";
    // g_setenv("GST_PLUGIN_PATH", pluginPath.toStdString().c_str(), TRUE);
    // qDebug() << "Looking for plugins in:" << pluginPath;


    // //gst_init(NULL, NULL); // Инициализация GStreamer

    // qDebug() << "Looking for plugins in:" << pluginPath;


    initIcons();  // инициализация иконок после создания приложения
    MainWindow w;
    w.show();
    return a.exec();
}
