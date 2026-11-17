#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <windows.h>
#include <QMainWindow>
#include <QMessageBox>
#include <QFileDialog>
#include <QList>
#include <QPainter>
#include <thread>
#include <QTimer>
#include <QDir>
#include <QDebug>
#include <QMetaType>
#include <QtConcurrent/QtConcurrent>
#include <QFuture>
#include <QLabel>
#include <QLineEdit>
#include <QDialog>
#include <QInputDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QDoubleSpinBox>
#include <QProgressDialog>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm> // для std::transform
#include <cctype>    // для tolower()
#include <mutex>


// GStreamer headers
#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/gstpad.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <glib.h>


//  #include <gst/audio/gstaudiolevel.h>


QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

extern QIcon playIcon;
extern QIcon pauseIcon;
class AudioVisualizerWidget;



class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data);
    void updatePosition();
    void sliderPressed();
    void sliderReleased();

    AudioVisualizerWidget *visualizer = nullptr;
    ~MainWindow();


private slots:
    void on_pushButton_clicked();

    void on_pushButton_2_clicked();

    void on_action_triggered();

    void on_action_2_triggered();

    void on_action_3_triggered();

    void on_action_4_triggered();

    void on_onSave_clicked();

public slots:
    void onRmsLevelsUpdated(const QList<double> &levels);
    void startPositionTimer();
    void stopPositionTimer();
    void setEqualizerBand(int band, double gain);
    void check_element(const gchar* name) {
        GstElementFactory* factory = gst_element_factory_find(name);
        if (factory) {
            qDebug() << "Plugin" << name << "availabale";
            gst_object_unref(factory);
        } else {
            qDebug() << "Plugin" << name << "doesn't found!";
        }
    }


private:
    Ui::MainWindow *ui;
    GstElement *pipeline = nullptr;  // объявляем pipeline как член класса
    GstElement *source = nullptr;
    //GstElement *demuxer = nullptr;
    GstElement *decoder = nullptr;
    GstElement *volume = nullptr;
    GstElement *convert = nullptr;
    GstElement *resample = nullptr;
    GstElement *sink = nullptr;
    GstElement *level = nullptr;
    gint64 duration;    // длительность играющего трека
    QTimer *positionTimer;
    std::mutex pipelineMutex;
    GstElement* equalizer;
    GMainLoop *loop = nullptr;
    std::thread *mainLoopThread = nullptr;
    std::atomic<bool> shouldQuit = false;


    bool shouldStopPipeline = false;  // Флаг для остановки pipeline
    std::atomic<bool> stopRequested = false;
    QList<QFutureWatcher<void>*> futureWatchers; // Список всех watcher'ов

};

class AudioVisualizerWidget : public QWidget {  // класс для виджета визуализирующего RMS трека
    Q_OBJECT
public:
    explicit AudioVisualizerWidget(QWidget *parent = nullptr) : QWidget(parent) {}
    QList<double> rmsLevels;
    void setLevels(const QVector<double> &levels) {
        rmsLevels = levels;
        update(); // вызывает repaint и paintEvent
    }


protected:
    void paintEvent(QPaintEvent *event) override;

private:

};


#endif // MAINWINDOW_H
