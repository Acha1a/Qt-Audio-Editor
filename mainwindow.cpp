#include "mainwindow.h"
#include "ui_mainwindow.h"

QIcon playIcon;
QIcon pauseIcon;
QString fileName;

static void onPadAdded(GstElement *src, GstPad *newPad, gpointer data); // объявление функции создания нового pad

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{

    ui->setupUi(this);
    ui->pushButton->setIcon(playIcon);
    // GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    // std::thread mainLoopThread([loop]() {
    //     g_main_loop_run(loop);
    // });
    // mainLoopThread.detach();

    loop = g_main_loop_new(nullptr, FALSE);
    mainLoopThread = new std::thread([this]() {
        g_main_loop_run(loop); // Запуск главного цикла GStreamer
    });

    gst_init(NULL, NULL); // Инициализация GStreamer
    gst_debug_set_default_threshold(GST_LEVEL_DEBUG);


    check_element("filesrc");
    check_element("decodebin");
    check_element("autoaudiosink");

    // Создать новый виджет с тем же родителем
    visualizer = new AudioVisualizerWidget(ui->widget->parentWidget());

    // Добавить в layout или установить geometry, если layout нет
    visualizer->setGeometry(ui->widget->geometry());
    visualizer->show();

    ui->widget->hide();  // Скрываем виджет из Designer, чтобы не мешал

    // Таймер для обновления позиции слайдера
    positionTimer = new QTimer(this);
    positionTimer->setInterval(500);
    connect(positionTimer, &QTimer::timeout, this, &MainWindow::updatePosition);
    // Обработка перемотки слайдера
    connect(ui->horizontalSlider, &QSlider::sliderPressed, this, &MainWindow::sliderPressed);
    connect(ui->horizontalSlider, &QSlider::sliderReleased, this, &MainWindow::sliderReleased);

    // Собираем все слайдеры эквалайзера
    QList<QSlider*> eqSliders = {
        ui->eqBand0, ui->eqBand1, ui->eqBand2, ui->eqBand3, ui->eqBand4,
        ui->eqBand5, ui->eqBand6, ui->eqBand7, ui->eqBand8, ui->eqBand9
    };

    // Устанавливаем диапазон значений (-24dB до +12dB - типичный для эквалайзеров)
    for (auto* slider : eqSliders) {
        slider->setRange(-240, 120); // в десятых долях dB для более точного управления
        slider->setValue(0);
    }

    // Соединяем сигналы слайдеров с эквалайзером
    for (int i = 0; i < eqSliders.size(); ++i) {
        connect(eqSliders[i], &QSlider::valueChanged, [this, i](int value) {
            this->setEqualizerBand(i, value / 10.0); // преобразуем в dB
        });
    }

    pipeline = gst_pipeline_new("audio-editor");    // создание нового pipeline, через который будет проходить аудиопоток
    if (!pipeline) {
        g_printerr("Can't create pipeline\n");
        return;
    }
    source = gst_element_factory_make("filesrc", NULL); // элемент для связывания аудиофайла с pipeline
    decoder = gst_element_factory_make("decodebin", NULL);
    volume = gst_element_factory_make("volume", NULL);
    convert = gst_element_factory_make("audioconvert", NULL);   // конвертация аудиоформатов
    resample = gst_element_factory_make("audioresample", "resample");   // для проверки сэмплов аудиофайла
    sink = gst_element_factory_make("autoaudiosink", "audio_sink");
    level = gst_element_factory_make("level", "level");

    equalizer = gst_element_factory_make("equalizer-10bands", "equalizer");
    if (!equalizer) {
        g_printerr("Can't create equalizer\n");
        return;
    }

    gst_bin_add_many(GST_BIN(pipeline), source, decoder, volume, equalizer, level, convert, resample, sink, NULL);

    // Связываем элементы
    if (!gst_element_link_many(source, decoder, nullptr)) {
        qWarning("Failed to link source to decoder");
    }

    if (!gst_element_link_many(volume, equalizer, level, convert, resample, sink, nullptr)) {
        qWarning("Failed to link audio processing elements");
    }


    // Подключаем обработчик динамического пада для decodebin
    g_signal_connect(decoder, "pad-added", G_CALLBACK(onPadAdded), volume);

    g_object_set(level, "message", TRUE, "num-bands", 8, NULL);


    // Создаём шину сообщение для поток воспроизведения
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, &MainWindow::bus_call, this);
    gst_object_unref(bus);

    gst_element_set_state(pipeline, GST_STATE_READY);
}

MainWindow::~MainWindow()
{

     stopPositionTimer();

    delete visualizer;

    // // Остановить GStreamer pipeline
    // if (pipeline) {
    //     // Отправляем flush и eos, чтобы прервать воспроизведение
    //     gst_element_send_event(pipeline, gst_event_new_flush_start());
    //     gst_element_send_event(pipeline, gst_event_new_eos());
    //     // Устанавливаем NULL state
    //     gst_element_set_state(pipeline, GST_STATE_NULL);
    //     // Остановить главный цикл GStreamer
    //     if (loop) {
    //         g_main_loop_quit(loop);
    //         // Пытаемся завершить поток с таймаутом
    //         if (mainLoopThread && mainLoopThread->joinable()) {
    //             std::chrono::milliseconds timeout(500); // 0.5 секунды
    //             if (mainLoopThread && mainLoopThread->joinable()) {
    //                 qDebug() << "GStreamer loop thread joined successfully";
    //             } else {
    //                 qWarning() << "GStreamer thread did not finish in time";
    //                 // Если поток всё ещё работает, можно detach(), но это небезопасно
    //                 // mainLoopThread->detach();
    //             }
    //         }
    //         g_main_loop_unref(loop);
    //         loop = nullptr;
    //     }

    //     gst_object_unref(pipeline);
    //     pipeline = nullptr;
    // }

    // 1. Останавливаем background tasks
    stopRequested = true;

    for (auto watcher : futureWatchers) {
        if (watcher->isRunning()) {
            watcher->cancel();
            watcher->waitForFinished(); // Блокирующий вызов
        }
        watcher->deleteLater();
    }
    futureWatchers.clear();

    // 2. Останавливаем GStreamer pipeline
    if (pipeline) {
        gst_element_send_event(pipeline, gst_event_new_flush_start());
        gst_element_send_event(pipeline, gst_event_new_eos());
        gst_element_set_state(pipeline, GST_STATE_NULL);

        gst_object_unref(pipeline);
        pipeline = nullptr;
    }

    // 3. Останавливаем main loop GStreamer
    if (mainLoopThread && mainLoopThread->joinable()) {
        // Оповещаем GStreamer loop о выходе
        if (loop) {
            g_main_loop_quit(loop);
        }

        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        while (mainLoopThread->joinable() &&
               std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (mainLoopThread->joinable()) {
            qWarning() << "GStreamer thread did not finish in time, detaching...";
            mainLoopThread->detach();
        } else {
            qDebug() << "GStreamer thread joined successfully";
        }

        g_main_loop_unref(loop);
        loop = nullptr;
        delete mainLoopThread;
        mainLoopThread = nullptr;
    }
    // if (loop) {
    //     g_main_loop_quit(loop);

    //     if (mainLoopThread && mainLoopThread->joinable()) {
    //         if (mainLoopThread->try_join_for(std::chrono::milliseconds(500))) {
    //             qDebug() << "GStreamer thread joined successfully";
    //         } else {
    //             qWarning() << "GStreamer thread did not finish in time, detaching...";
    //             mainLoopThread->detach();
    //         }
    //     }


    // }

    delete ui;
}


void MainWindow::on_pushButton_clicked()
{   // кнопка воспроизведения/паузы
    std::lock_guard<std::mutex> lock(pipelineMutex);
    if (fileName.isEmpty()) {
        QMessageBox msgBox;
        msgBox.setText("File error!");
        msgBox.exec();
        return void();
    }

    GstState current_state;
    gst_element_get_state(pipeline, &current_state, NULL, GST_CLOCK_TIME_NONE);

    if (!ui->pushButton->isChecked()) {
        // gst_element_seek(pipeline, 1.0, GST_FORMAT_TIME,
        //                  GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, 0,
        //                  GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
        gst_element_set_state(pipeline, GST_STATE_PAUSED);
        ui->pushButton->setIcon(playIcon);  // показываем иконку воспроизведения
        //positionTimer->stop();

    } else {
        // Добавляем watch на bus для получения сообщений level
        if (current_state == GST_STATE_PAUSED){
            gst_element_set_state(pipeline, GST_STATE_PLAYING);
        }
        else
            gst_element_set_state(pipeline, GST_STATE_PLAYING);


        ui->pushButton->setIcon(pauseIcon);   // показываем иконку паузы
    }
}



void MainWindow::on_pushButton_2_clicked()
{   // кнопка открытия аудиофайла
    //std::lock_guard<std::mutex> lock(pipelineMutex);
    fileName = QFileDialog::getOpenFileName(this, tr("Open audiofile"),
                                            QString(),
                                            tr("Formats (*.wav *.raw *.aiff *.aifc *.au *.mp3 *.flac *.ogg *.opus *.gsm *.spx *.wma *.m4a)"));
    if (fileName.isEmpty()) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Файл не выбран"));
        return;
    }

    // Остановить текущий поток
    gst_element_set_state(pipeline, GST_STATE_READY);

    ui->pushButton->setIcon(playIcon);
    ui->pushButton->setChecked(false);

    g_object_set(G_OBJECT(source), "location", fileName.toUtf8().constData(), NULL);
    qDebug() << "Set filesrc location to:" << fileName;

    // ВАЖНО: pipeline должен быть хотя бы в PAUSED!
    gst_element_set_state(pipeline, GST_STATE_PAUSED);

    // Ждем, пока пайплайн действительно перейдет в PAUSED
    GstStateChangeReturn ret = gst_element_get_state(pipeline, NULL, NULL, 5 * GST_SECOND);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        qWarning() << "Failed to pause pipeline";
        return;
    }

    // Теперь можно запрашивать длительность
    gint64 duration_ns;
    if (gst_element_query_duration(pipeline, GST_FORMAT_TIME, &duration_ns)) {
        int duration_ms = duration_ns / GST_MSECOND;
        ui->horizontalSlider->setRange(0, duration_ms);
        ui->horizontalSlider->setValue(0); // Сбрасываем позицию в начало
    } else {
        qWarning() << "Could not query duration";
        ui->horizontalSlider->setRange(0, 100); // Устанавливаем дефолтный диапазон
    }

    //updatePosition();

    ui->horizontalSlider->setValue(0);
}


static void onPadAdded(GstElement *src, GstPad *newPad, gpointer data) {    // функция для построения pipeline для вывода аудио
    // для decodebin нужен обработчик сигнала, чтобы он знал какие данные имееют место в потоке
    // В момент появления нового pad вызывается onPadAdded, в котором определяется тип данных (например, аудио или видео) и
    // связывается новый pad с соответствующим входным pad следующего элемента.

    // параметр src не используется в данной реализации потому что нет нужды в обращении к decoder
    Q_UNUSED(src);
    GstElement *target_element = static_cast<GstElement*>(data);
    GstPad *sink_pad = gst_element_get_static_pad(target_element, "sink");

    if (gst_pad_is_linked(sink_pad)) {
        gst_object_unref(sink_pad);
        return;
    }

    // Получаем caps (сначала текущие, если их нет — запрашиваем)
    GstCaps *new_pad_caps = gst_pad_get_current_caps(newPad);
    if (!new_pad_caps)
        new_pad_caps = gst_pad_query_caps(newPad, nullptr);

    const gchar *media_type = gst_structure_get_name(gst_caps_get_structure(new_pad_caps, 0));

    // Проверяем, аудио ли это
    if (g_str_has_prefix(media_type, "audio/")) {  // или "audio/x-raw" для более строгой проверки
        GstPadLinkReturn ret = gst_pad_link(newPad, sink_pad);
        if (ret != GST_PAD_LINK_OK) {
            g_warning("Failed to link pads: %s", gst_pad_link_get_name(ret));
        } else {
            g_debug("Linked pad %s to %s", media_type, GST_ELEMENT_NAME(target_element));
        }
    } else {
        g_debug("Ignoring non-audio pad: %s", media_type);
    }

    gst_caps_unref(new_pad_caps);
    gst_object_unref(sink_pad);
}


gboolean MainWindow::bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    Q_UNUSED(bus);  // подавляем предупреждение
    MainWindow *self = static_cast<MainWindow*>(data);

    //qDebug() << "Message type:" << GST_MESSAGE_TYPE_NAME(msg);

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ELEMENT: {
        const GstStructure *s = gst_message_get_structure(msg);
        const GValue *val = gst_structure_get_value(s, "rms");
        if (val && G_VALUE_HOLDS(val, G_TYPE_VALUE_ARRAY)) {
            const GValueArray *array_const = static_cast<const GValueArray *>(g_value_get_boxed(val));
            GValueArray *array = const_cast<GValueArray *>(array_const);
            QList<double> levels;
            for (guint i = 0; i < array->n_values; ++i) {
                const GValue *item = g_value_array_get_nth(array, i);
                levels.append(g_value_get_double(item));
            }
            QMetaObject::invokeMethod(self, "onRmsLevelsUpdated",
                                      Qt::BlockingQueuedConnection,
                                      Q_ARG(QList<double>, levels));
        }
        break;
    }
    case GST_MESSAGE_ERROR: {
        GError *err;
        gchar *debug;
        gst_message_parse_error(msg, &err, &debug);
        qWarning() << "GStreamer error:" << err->message;
        g_error_free(err);
        g_free(debug);
        break;
    }
    case GST_MESSAGE_EOS: {
        qDebug() << "End of stream";
        // Можно остановить pipeline или выполнить другие действия
        break;
    }
    case GST_MESSAGE_STATE_CHANGED: {
        GstState old_state, new_state;
        gst_message_parse_state_changed(msg, &old_state, &new_state, nullptr);
        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(self->pipeline)) {
            if (new_state == GST_STATE_PLAYING) {
                QMetaObject::invokeMethod(self, "startPositionTimer", Qt::BlockingQueuedConnection); // добавляем запуск таймера в очередь потока
            } else {
                QMetaObject::invokeMethod(self, "stopPositionTimer", Qt::BlockingQueuedConnection);  // добавляем остановку таймера в очередь потока
            }
        }
        break;
    }
    case GST_MESSAGE_ASYNC_DONE: {
        QMetaObject::invokeMethod(self, "startPositionTimer", Qt::BlockingQueuedConnection);
        break;
    }
    default:
        break;
    }

    return TRUE;
}

// Слот для обновления rmsLevels и перерисовки
void MainWindow::onRmsLevelsUpdated(const QList<double> &levels) {
    //qDebug() << "rmsLevels size:" << visualizer->rmsLevels.size() << "values:" << visualizer->rmsLevels;
    visualizer->rmsLevels = levels;

    update(); // вызывает paintEvent для перерисовки виджета
    //qDebug() << "rmsLevels size:" << visualizer->rmsLevels.size() << "values:" << visualizer->rmsLevels;
}

void AudioVisualizerWidget::paintEvent(QPaintEvent *) {
    // отрисовка виджета для RMS уровней
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if (rmsLevels.empty()) return;

    //qDebug() << "rmsLevels size:" << rmsLevels.size() << "values:" << rmsLevels;

    painter.setPen(Qt::green);
    int w = width();
    int h = height();

    // Количество каналов
    int channelCount = rmsLevels.size();

    // Ширина и позиция каждого столбца
    int barWidth = w / (channelCount * 2); // Оставляем промежутки между столбцами
    int spacing = barWidth / 2;
    int totalBarWidth = channelCount * barWidth;
    int totalSpacing = (channelCount - 1) * spacing;
    int totalWidthNeeded = totalBarWidth + totalSpacing;

    // Центрируем группу столбцов
    int startX = (w - totalWidthNeeded) / 2;

    QVector<QColor> channelColors = {Qt::green, Qt::yellow, Qt::red, Qt::blue, Qt::cyan, Qt::white, Qt::magenta, Qt::darkRed};
    for (int i = 0; i < channelCount; ++i) {
        painter.setPen(channelColors[i % channelColors.size()]);
        // Позиция по X (центрируем)
        //int x = (w * (i + 1)) / (channelCount + 1) - barWidth / 2;
        int x = startX + i * (barWidth + spacing);
        // Высота столбца (нормализуем от -60dB до 0dB)
        int height = qBound(0, int((rmsLevels[i] + 60) * h / 60), h);


        // Рисуем столбец
        painter.drawRect(x, h - height, barWidth, height);

        // Подпись канала (опционально)
        painter.drawText(x, h - height - 5, QString("Ch %1").arg(i + 1));
    }
}

void MainWindow::updatePosition() { // функция для обновления слайдера и таймера
    if (!pipeline || !ui) return;  // Проверка на существование объектов
    std::lock_guard<std::mutex> lock(pipelineMutex);
    GstState state;
    gst_element_get_state(pipeline, &state, nullptr, 0);
    if (state != GST_STATE_PLAYING && state != GST_STATE_PAUSED) {
        return; // Не обновляем, если пайплайн не активен
    }
    if (state == GST_STATE_PLAYING || state == GST_STATE_PAUSED) {
        gint64 pos = 0;
        if (gst_element_query_position(pipeline, GST_FORMAT_TIME, &pos)) {
            qDebug() << "Position (ms):" << pos / GST_MSECOND;
            // Обновляем QTimeEdit
            int ms = static_cast<int>(pos / GST_MSECOND);
            if (!ui->horizontalSlider->isSliderDown()) { // Обновляем только если пользователь не двигает слайдер
            // Преобразуем миллисекунды в QTime
            QTime time = QTime(0, 0).addSecs(ms / 1000);  // добавляем секунды
            ui->timeEdit->setTime(time);
            // Обновляем слайдер только если пользователь не перемещает его вручную
            if (!ui->horizontalSlider->isSliderDown()) {
                ui->horizontalSlider->setValue(static_cast<int>(pos / GST_MSECOND));
                qDebug() << "updatePosition called";
                }
            }
        }
        else {
            qDebug() << "Failed to query position";
        }
    }
}

void MainWindow::sliderPressed() {
    // Останавливаем обновление позиции во время перемещения слайдера
    QMetaObject::invokeMethod(this, "stopPositionTimer", Qt::BlockingQueuedConnection);
}

void MainWindow::sliderReleased() {
    std::lock_guard<std::mutex> lock(pipelineMutex);
    // Перематываем аудио на позицию слайдера
    gint64 newPos = ui->horizontalSlider->value() * GST_MSECOND;
    // находим новую позицию таймера
    // gst_element_seek(pipeline, 1.0, GST_FORMAT_TIME,
    //                  static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
    //                  GST_SEEK_TYPE_SET, newPos,
    //                  GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);

    gst_element_seek(pipeline, 1.0, GST_FORMAT_TIME,
                     static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
                     GST_SEEK_TYPE_SET, newPos,
                     GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    // Возобновляем таймер обновления позиции
    QMetaObject::invokeMethod(this, "startPositionTimer", Qt::BlockingQueuedConnection);
}


void initIcons()
{   // для инициализации иконок после запуска самого приложения, иначе ошибка
    pauseIcon = QIcon("Pause_Button.png");  // инициализация после создания QApplication
    playIcon = QIcon("Play_Button.png");  // инициализация после создания QApplication
    return void();
}

void MainWindow::startPositionTimer(){
    duration = 0;
    if (gst_element_query_duration(pipeline, GST_FORMAT_TIME, &duration) && duration > 0) {
        int msDuration = static_cast<int>(duration / GST_MSECOND);
        // Устанавливаем максимальное время в QTimeEdit
        QTime maxTime = QTime(0, 0).addSecs(msDuration / 1000);
        //ui->timeEdit->setDisplayFormat("mm:ss"); // Формат минуты:секунды
        ui->timeEdit->setMaximumTime(maxTime);
        // Обновляем слайдер
        ui->horizontalSlider->setMaximum(msDuration);
        ui->horizontalSlider->setRange(0, msDuration);
        ui->horizontalSlider->setEnabled(true);
    }
    positionTimer->start();
}
void MainWindow::stopPositionTimer(){
    positionTimer->stop();
}

void MainWindow::setEqualizerBand(int band, double gain) {
    if (band < 0 || band >= 10) return;    // проверям, что полоса 1 из 10 доступных

    std::lock_guard<std::mutex> lock(pipelineMutex);
    if (!equalizer) return;

    QByteArray prop = QString("band%1").arg(band).toUtf8();     // формируем запись полосы эквалайзера
    g_object_set(G_OBJECT(equalizer), prop.constData(), gain, nullptr);     // устанавлиявает конкретное значение полосы у эквалайзера
}

// Функция для получения расширения файла в нижнем регистре
std::string get_file_extension(const std::string& filename) {
    size_t dot_pos = filename.find_last_of('.');
    if (dot_pos == std::string::npos) return "";
    std::string ext = filename.substr(dot_pos + 1);
    for (auto & c : ext) c = tolower(c);
    return ext;
}

// Функция выбора энкодера и расширения по расширению файла
struct EncoderInfo {
    const char* encoder_name;
    const char* muxer_name;
    const char* file_extension;
};

EncoderInfo choose_encoder_by_extension(const std::string& ext) {
    if (ext == "wav") return { "wavenc", nullptr, "wav" };
    if (ext == "raw") return { "wavenc", nullptr, "raw" };
    if (ext == "aiff" || ext == "aifc") return { "wavenc", nullptr, "aiff" };
    if (ext == "au") return { "wavenc", nullptr, "au" };
    if (ext == "mp3") return { "lamemp3enc", nullptr, "mp3" }; // id3v2mux опционален
    if (ext == "flac") return { "flacenc", nullptr, "flac" };
    if (ext == "ogg") return { "vorbisenc", "oggmux", "ogg" };
    if (ext == "opus") return { "opusenc", "oggmux", "opus" };
    if (ext == "spx") return { "speexenc", "oggmux", "spx" };
    if (ext == "wma") return { "wavenc", nullptr, "wma" };
    if (ext == "m4a") return { "avenc_aac", "mp4mux", "m4a" }; // если faac не доступен

    return { "wavenc", nullptr, "wav" };
}


void cleanup_buffers_and_pipeline(std::vector<GstBuffer*>& buffers, GstElement* pipeline, GstCaps* caps) {
    for (auto buf : buffers) gst_buffer_unref(buf);
    if (caps) gst_caps_unref(caps);
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }
}

void cleanup_pipeline(GstElement* pipeline, GstCaps* caps) {    // освобождаем память отработавшего конвейера
    if (caps) gst_caps_unref(caps);
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }
}

void MainWindow::on_action_triggered()
{   // вырезка фрагмента из аудиофайла для отдельного сохранения
    fileName = QFileDialog::getOpenFileName(this, tr("Open audiofile"),
                                            QString(),
                                            tr("Formats (*.wav *.raw *.aiff *.aifc *.au *.mp3 *.flac *.ogg *.opus *.gsm *.spx *.wma *.m4a)"));

    if (!fileName.isEmpty()) {
        // аудиофайл определён
        QString savePath = QFileDialog::getSaveFileName(
            nullptr,
            "Сохранить фрагмент",
            "",
            "*.wav;; *.raw;; *.aiff;; *.aifc;; *.au;; *.mp3;; *.flac ;;*.ogg;; *.opus;; *.gsm;; *.spx;; *.wma;; *.m4a"
            );
        if (savePath.isEmpty()) return; // путь сохранения не определён


        // Диалог для выбора времени начала и конца
        QDialog dialog(this);
        dialog.setWindowTitle("Select Start and End Time"); // заголовок окошка для выбора временного промежутка

        QLabel *startLabel = new QLabel("Start time (seconds):");
        QLabel *endLabel = new QLabel("End time (seconds):");
        QLineEdit *startTimeEdit = new QLineEdit("0");  // предустановка в "0" секунд
        QLineEdit *endTimeEdit = new QLineEdit("5");    // предустановка в "5" секунд

        QHBoxLayout *timeLayout = new QHBoxLayout();    // горизонтальный компоновщик
        timeLayout->addWidget(startLabel);  // добавляем виджеты на окошко
        timeLayout->addWidget(startTimeEdit);
        timeLayout->addWidget(endLabel);
        timeLayout->addWidget(endTimeEdit);

        QVBoxLayout *mainLayout = new QVBoxLayout(&dialog); // вертикальный компоновщик
        mainLayout->addLayout(timeLayout);  // вертикальный компоновщик будет основным

        QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        mainLayout->addWidget(buttonBox);   // добавляем кнопки на окошко

        connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); // связываем кнопку и событие
        connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        if (dialog.exec() != QDialog::Accepted) {   // прерываем функцию сразу, т. к., путь сохранения получить не удалось
            return;
        }

        // проверяем, что введены именно вещественные числа
        bool okStart, okEnd;
        double startSec = startTimeEdit->text().toDouble(&okStart);
        double endSec = endTimeEdit->text().toDouble(&okEnd);



        if (!okStart || !okEnd || startSec < 0 || endSec <= startSec) {
            QMessageBox::critical(this, "Error", "Invalid time values!\nStart must be less than end and >= 0.");
            return;
        }

        if (startSec < 0 || endSec < 0) {
            QMessageBox::critical(this, "Ошибка", "Время не может быть отрицательным.");
            return;
        }

        if (startSec >= endSec) {
            QMessageBox::critical(this, "Ошибка", "Начальная позиция должна быть меньше конечной.");
            return;
        }


        // преобразование переменных времени из секунд в наносекунды
        GstClockTime start_ns = static_cast<GstClockTime>(startSec * GST_SECOND);
        GstClockTime end_ns = static_cast<GstClockTime>(endSec * GST_SECOND);



        // --- Pipeline для вырезки аудио ---

        QFuture<void> future = QtConcurrent::run([=]() {
            QString savePathStr = savePath; // берём копию строки пути сохранения результата
            std::string savePathStd = savePathStr.toStdString();   // преобразуем в сишную строку


            GstElement *pipeline1 = gst_pipeline_new("audio-cut");
            GstElement *source1 = gst_element_factory_make("filesrc", "source1");
            GstElement *decode1 = gst_element_factory_make("decodebin", "decode1");
            GstElement *convert1 = gst_element_factory_make("audioconvert", "convert1");
            // "audioconvert" - отвечает за преобразование аудиоданных между разными форматами (например, изменение формата с одного аудиокодека на другой, изменение количества каналов,
            // частоты дискретизации и т.п.)
            // "convert1" — имя, которое присваивается этому элементу в конвейере
            GstElement *resample1 = gst_element_factory_make("audioresample", "resample1");
            GstElement *sink1 = gst_element_factory_make("appsink", "sink1");
            // "appsink" — это специальный элемент, который принимает поток данных из конвейера и передаёт их в приложение для дальнейшей обработки (например, анализ, сохранение, отображение).
            // "sink1" — имя элемента в конвейере, которое позволяет его идентифицировать

            if (!pipeline1 || !source1 || !decode1 || !convert1 || !resample1 || !sink1) {
                qCritical() << "Can't create pipeline1 elements";
                if (pipeline1) gst_object_unref(pipeline1);
                return;
            }

            g_object_set(source1, "location", fileName.toUtf8().constData(), NULL);
            g_object_set(sink1,
                         "emit-signals", FALSE, // Отключает генерацию сигналов при поступлении новых буферов
                         "sync", FALSE, // Отключает синхронизацию по времени. Элемент не будет ждать синхронизации с тактовым сигналом
                         "drop", TRUE,  // Включает сброс буферов, если они не успевают обрабатываться. Для избежания проблем с буферизацией
                         "max-buffers", 100,    // Ограничивает максимальное количество буферов, которые appsink может хранить в очереди
                         NULL);    // обозначение конца списка свойств


            // добавляем источник, декодер, конвертер, ресэмпл и приёмник в конвейер
            gst_bin_add_many(GST_BIN(pipeline1), source1, decode1, convert1, resample1, sink1, NULL);

            // Линкуем источник с декодером
            if (!gst_element_link(source1, decode1)) {
                qCritical() << "Error linking source1 & decode1";
                gst_object_unref(pipeline1);
                return;
            }

            // Динамическое связывание пада с декодером
            g_signal_connect(decode1, "pad-added", G_CALLBACK(+[](GstElement *src, GstPad *pad, gpointer data){
                                 GstElement *convert = GST_ELEMENT(data);
                                 GstPad *sinkpad = gst_element_get_static_pad(convert, "sink"); // приём данных

                                 if (gst_pad_is_linked(sinkpad)) {
                                     gst_object_unref(sinkpad);
                                     return;
                                 }

                                 GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
                                 if (ret != GST_PAD_LINK_OK) {
                                     qCritical() << "Failed to link pads:" << ret;
                                 }
                                 gst_object_unref(sinkpad);
                             }), convert1);




            // Теперь связываем остальные элементы
            if (!gst_element_link_many(convert1, resample1, sink1, NULL)) {
                qCritical() << "Error linking convert1, resample1, sink1";
                gst_object_unref(pipeline1);   // убираем ссылку, т. к., пайплайн неудачен
                return;
            }

            // запускаем пайплайн
            GstStateChangeReturn state_ret = gst_element_set_state(pipeline1, GST_STATE_PLAYING);
            if (state_ret == GST_STATE_CHANGE_FAILURE) {
                qCritical() << "Can't set pipeline1 to PLAYING";
                gst_object_unref(pipeline1);
                return;
            }

            // оцениваем функциональность пайплайна - прогрузился пайплайн или нет
            state_ret = gst_element_get_state(pipeline1, nullptr, nullptr, 5 * GST_SECOND);
            if (state_ret == GST_STATE_CHANGE_FAILURE) {
                qCritical() << "Failed to complete state change to PLAYING";
                gst_element_set_state(pipeline1, GST_STATE_NULL);
                gst_object_unref(pipeline1);
                return;
            }


            // проверяем - есть ли начальный сэмпл
            qDebug() << "Trying to pull first sample...";
            GstSample *probe_sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink1), 5 * GST_SECOND);
            if (!probe_sample) {
                qCritical() << "Can't get sample from sink1";
                gst_element_set_state(pipeline1, GST_STATE_NULL);
                gst_object_unref(pipeline1);
                return;
            }


            // проверка - можно ли настроить caps для этого пайплайна
            GstCaps *caps_sink = gst_sample_get_caps(probe_sample);
            if (!caps_sink) {
                qCritical() << "Caps are null!";
                gst_sample_unref(probe_sample);
                gst_element_set_state(pipeline1, GST_STATE_NULL);
                gst_object_unref(pipeline1);
                return;
            }

            // получаем параметры аудио
            GstStructure *str = gst_caps_get_structure(caps_sink, 0);
            int sample_rate = 96000, channels = 2;  // значения по умолчанию
            gst_structure_get_int(str, "rate", &sample_rate);   // попытка записи частоты дискретизации
            gst_structure_get_int(str, "channels", &channels);  // попытка записи аудиоканалов

            const char* fmt = gst_structure_get_string(str, "format");  // попытка получить информацию о способе представления сэмплов в памяти
            if (!fmt) {
                qCritical() << "Failed to get format from caps";
                gst_sample_unref(probe_sample);
                gst_element_set_state(pipeline1, GST_STATE_NULL);
                gst_object_unref(pipeline1);
                return;
            }

            // для итогового пайплайна создаём свои caps c такими же параметрами
            GstCaps* raw_caps = gst_caps_new_simple(
                "audio/x-raw",  // необработанное аудио
                "format", G_TYPE_STRING, fmt,
                "rate", G_TYPE_INT, sample_rate,
                "channels", G_TYPE_INT, channels,
                "layout", G_TYPE_STRING, "interleaved", // указывает, что аудиоданные расположены в интерливированном виде (каналы чередуются в одном потоке)
                NULL);

            // попытка установить позицию воспроизведения на время start_ns максимально точно
            if (!gst_element_seek_simple(pipeline1, GST_FORMAT_TIME,
                                         static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE), start_ns)) {
                qCritical() << "Seek Error!";
                // чистим память
                gst_sample_unref(probe_sample);
                gst_caps_unref(raw_caps);
                gst_element_set_state(pipeline1, GST_STATE_NULL);
                gst_object_unref(pipeline1);
                return;
            }

            // попытка установить позицию воспроизведения на время end_ns максимально точно
            if (!gst_element_seek_simple(pipeline1, GST_FORMAT_TIME,
                                         static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE), end_ns)) {
                qCritical() << "Seek Error!";
                // чистим память
                gst_sample_unref(probe_sample);
                gst_caps_unref(raw_caps);
                gst_element_set_state(pipeline1, GST_STATE_NULL);
                gst_object_unref(pipeline1);
                return;
            }

            gst_element_seek_simple(pipeline1, GST_FORMAT_TIME, static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE), start_ns);

            // собираем буферы
            std::vector<GstBuffer*> buffers;   // вектор для последовательности буферов
            gint64 current_pos = start_ns;  // задаём начальную позицию вырезки
            bool error_occurred = false;

            // запускаем буферизацию пока не дошли до конечной точки вырезки, конца файла, или не произошла ошибка
            while (current_pos < end_ns && !error_occurred && !gst_app_sink_is_eos(GST_APP_SINK(sink1))) {
                GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink1));  // работаем над конкретным сэмплом
                if (!sample) {  // если что-то не так с сэмплом
                    qCritical() << "Failed to pull sample";
                    error_occurred = true;
                    break;
                }


                // буфер - минимальной блок информации аудиофайла
                GstBuffer *buffer = gst_sample_get_buffer(sample);  // получаем буфер
                if (!buffer) {  // если что-то не так с буфером этого сэмпла
                    qCritical() << "Sample has no buffer";
                    gst_sample_unref(sample);
                    error_occurred = true;
                    break;
                }

                GstClockTime buf_start = GST_BUFFER_PTS(buffer);    // получаем начало буфера
                GstClockTime buf_duration = GST_BUFFER_DURATION(buffer);    // узнаём длительность буфера

                if (buf_start == GST_CLOCK_TIME_NONE || buf_duration == GST_CLOCK_TIME_NONE) {  // если временных меток нету
                    qWarning() << "Buffer has no timestamp or duration, using copy";
                    GstBuffer *copy = gst_buffer_copy(buffer);  // копируем весь буфер
                    if (copy) {
                        buffers.push_back(copy);
                    }
                }
                else if (buf_start < start_ns && buf_start + buf_duration > start_ns){
                    guint64 buf_size = gst_buffer_get_size(buffer);
                    guint64 copy_size = (buf_start + buf_duration - start_ns) * buf_size / GST_BUFFER_DURATION(buffer);
                    if (copy_size > 0) {
                        GstBuffer *cut_buf = gst_buffer_copy_region(buffer, GST_BUFFER_COPY_ALL, 0, copy_size);
                        if (cut_buf) buffers.push_back(cut_buf);
                    }
                }
                else if (buf_start + buf_duration > end_ns) {   // если буфер выходит за end_ns
                    // Нужно взять только часть этого буфера
                    guint64 cut_ns = end_ns - buf_start;    // длительность части буфера, которую берём
                    gsize buffer_size = gst_buffer_get_size(buffer);    // узнаём, сколько весит буфер, с которым работаем
                    gsize size_to_copy = (gsize)((double)cut_ns / buf_duration * buffer_size);  // определяем размер нужной части буфера

                    GstBuffer* cut_buf = gst_buffer_copy_region(buffer, GST_BUFFER_COPY_ALL, 0, size_to_copy);  // создаём в памяти новый буфер, финализирующий последовательность
                    if (cut_buf) {  // если последний буфер создался успешно, то добавляем его в последовательность
                        buffers.push_back(cut_buf);
                    }
                }
                else {  // если так получилось, что последний буфер тютелька в тютельку состыковался с окончанием вырезки, то добавляем его целиком
                    GstBuffer *copy = gst_buffer_copy(buffer);
                    if (copy) {
                        buffers.push_back(copy);
                    }
                }

                // если временные метки в норме, то обновляем позицию, на которой находимся
                if (buf_start != GST_CLOCK_TIME_NONE && buf_duration != GST_CLOCK_TIME_NONE) {
                    current_pos = buf_start + buf_duration;
                }

                gst_sample_unref(sample);   // освобождаем сэмпл
            }

            // Чистим память
            gst_sample_unref(probe_sample);
            gst_element_set_state(pipeline1, GST_STATE_NULL);  // останавливаем работу пайплайна
            gst_object_unref(pipeline1);

            if (error_occurred || buffers.empty()) {    // если что-то не так, не получилось создать коллекцию буферов
                qCritical() << "Failed to collect buffers or no data available";
                for (auto buf : buffers) gst_buffer_unref(buf);
                gst_caps_unref(raw_caps);
                return;
            }

            // Создание нового пайплайна для создания итогового аудиофайла
            std::string ext = get_file_extension(savePathStd);  // расширение, в котором пользователь хочет сохранить
            EncoderInfo encInfo = choose_encoder_by_extension(ext);

            GstElement* pipeline2 = gst_pipeline_new("audio-save");
            GstElement* appsrc = gst_element_factory_make("appsrc", "appsrc");
            GstElement* convert2 = gst_element_factory_make("audioconvert", "convert2");
            GstElement* resample2 = gst_element_factory_make("audioresample", "resample2");
            GstElement* encoder = gst_element_factory_make(encInfo.encoder_name, "encoder");
            GstElement* muxer = encInfo.muxer_name ? gst_element_factory_make(encInfo.muxer_name, "muxer") : nullptr;   // муксер может и не нужен
            GstElement* filesink = gst_element_factory_make("filesink", "filesink");

            if (!pipeline2 || !appsrc || !convert2 || !resample2 || !encoder || !filesink || (encInfo.muxer_name && !muxer)) {
                qCritical() << "Can't create pipeline2 elements";
                for (auto buf : buffers) gst_buffer_unref(buf);
                gst_caps_unref(raw_caps);
                if (pipeline2) gst_object_unref(pipeline2);
                return;
            }

            // задаём appsrc
            g_object_set(appsrc,
                         "caps", raw_caps,  // берём установленные капсы
                         "format", GST_FORMAT_TIME, // формат позиций и смещений в источнике задаётся во времени (наносекунды)
                         "stream-type", GST_APP_STREAM_TYPE_STREAM, // определяет тип потока как непрерывный (stream), то есть данные подаются последовательно, без заранее известной длины
                         "is-live", FALSE,  // поток не является живым (live), то есть данные не поступают в реальном времени, а могут быть заранее подготовлены
                         NULL);

            // Если это MP3 расширение
            if (strstr(encInfo.encoder_name, "lamemp3")) {
                g_object_set(encoder, "target", 1, "bitrate", 128, NULL); // Определяем битрейт для MP3
            }

            g_object_set(filesink, "location", savePathStd.c_str(), NULL);  // устанавливаем место сохранения файла

            // На случай, если фигурирует мультиплексирование, но в данном случае такое случается редко
            if (muxer) {
                gst_bin_add_many(GST_BIN(pipeline2), appsrc, convert2, resample2, encoder, muxer, filesink, NULL);
                if (!gst_element_link_many(appsrc, convert2, resample2, encoder, muxer, filesink, NULL)) {
                    qCritical() << "Error linking pipeline2 with muxer";
                    cleanup_buffers_and_pipeline(buffers, pipeline2, raw_caps);
                    return;
                }
            } else {    // без мультиплексирования
                gst_bin_add_many(GST_BIN(pipeline2), appsrc, convert2, resample2, encoder, filesink, NULL);
                if (!gst_element_link_many(appsrc, convert2, resample2, encoder, filesink, NULL)) {
                    qCritical() << "Error linking pipeline2";
                    cleanup_buffers_and_pipeline(buffers, pipeline2, raw_caps);
                    return;
                }
            }

            // Запускаем пайплайн
            state_ret = gst_element_set_state(pipeline2, GST_STATE_PLAYING);
            if (state_ret == GST_STATE_CHANGE_FAILURE) {
                qCritical() << "Can't set pipeline2 to PLAYING";
                cleanup_buffers_and_pipeline(buffers, pipeline2, raw_caps);
                return;
            }

            // Загоняем буферы в appsrc
            for (auto buf : buffers) {
                GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buf); // элемент, который сообщает фреймворку и другим элементам, как прошла операция с буфером
                if (ret != GST_FLOW_OK) {
                    qCritical() << "Error pushing buffer to appsrc:" << ret;
                    error_occurred = true;
                    break;
                }
            }
            buffers.clear();    // чистим контейнер с буферами

            if (error_occurred) {
                cleanup_pipeline(pipeline2, raw_caps);
                return;
            }

            // Отправляем сообщение, что достигнут конец потока
            gst_app_src_end_of_stream(GST_APP_SRC(appsrc));

            // Создаём шину сообщений. Шина используется для передачи сообщений от элементов пайплайна к приложению
            GstBus *bus = gst_element_get_bus(pipeline2);
            //  Создаём сообщение, которое может быть либо об окончании потока, либо об ошибке
            GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
                                                         static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));

            if (msg) {  // Если сообщение пришло
                switch (GST_MESSAGE_TYPE(msg)) {
                case GST_MESSAGE_ERROR: {   // Если это ошибка
                    GError *err = nullptr;
                    gchar *dbg = nullptr;   // char Glib для совместимости с UTF-8
                    gst_message_parse_error(msg, &err, &dbg);   // получаем текст ошибки
                    qCritical() << "Error saving file:" << err->message;    // выводим текст ошибки
                    if (dbg) qCritical() << "Debug info:" << dbg;   // если есть отладочная информация
                    g_error_free(err);
                    g_free(dbg);
                    break;
                }
                case GST_MESSAGE_EOS:   // Если это окончание потока, значит - файл успешно сохранён
                    qInfo() << "Successfully saved audio to" << savePathStr;
                    break;
                default:
                    break;
                }
                gst_message_unref(msg);
            }

            // Чистим память
            gst_object_unref(bus);
            gst_element_set_state(pipeline2, GST_STATE_NULL);
            gst_object_unref(pipeline2);
            gst_caps_unref(raw_caps);
        });

        QFutureWatcher<void>* watcher = new QFutureWatcher<void>(this);
        connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher]() {
            watcher->deleteLater();
            futureWatchers.removeAll(watcher);
        });
        watcher->setFuture(future);
        futureWatchers.append(watcher);


    } else {
        QMessageBox msgBox;
        msgBox.setText("Изначальный трек не выбран!");
        msgBox.exec();
        return void();
        // pad отсутствует, источник не готов
    }

}


void MainWindow::on_action_2_triggered()
{   // удаление фрагмента из аудиофайла
    // Выбор исходного файла
    QString fileName = QFileDialog::getOpenFileName(this, tr("Open audio file"),
                                                    QString(),
                                                    tr("Audio Files (*.wav *.raw *.aiff *.aifc *.au *.mp3 *.flac *.ogg *.opus *.gsm *.spx *.wma *.m4a)"));
    if (fileName.isEmpty()) return;

    // Выбор пути для сохранения
    // QString savePath = QFileDialog::getSaveFileName(this, "Save trimmed file", "",
    //                                                 "*.wav;;*.mp3;;*.ogg;;*.opus;;*.flac");
    // if (savePath.isEmpty()) return;

    QString savePath = QFileDialog::getSaveFileName(
        nullptr,
        "Сохранить обрезанный файл",
        "",
        "*.wav;; *.raw;; *.aiff;; *.aifc;; *.au;; *.mp3;; *.flac ;;*.ogg;; *.opus;; *.gsm;; *.spx;; *.wma;; *.m4a"
        );
    if (savePath.isEmpty()) return; // путь сохранения не определён

    // Диалог для ввода времени удаления
    QDialog dialog(this);
    dialog.setWindowTitle("Select time to remove");

    QFormLayout *formLayout = new QFormLayout(&dialog);

    QDoubleSpinBox *startTimeEdit = new QDoubleSpinBox();
    startTimeEdit->setRange(0, 999999);
    startTimeEdit->setValue(5);
    startTimeEdit->setSingleStep(0.1);

    QDoubleSpinBox *endTimeEdit = new QDoubleSpinBox();
    endTimeEdit->setRange(0, 999999);
    endTimeEdit->setValue(10);
    endTimeEdit->setSingleStep(0.1);

    formLayout->addRow("Start time (seconds):", startTimeEdit);
    formLayout->addRow("End time (seconds):", endTimeEdit);

    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    formLayout->addRow(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;

    double startSec = startTimeEdit->value();
    double endSec = endTimeEdit->value();

    if (startSec < 0 || endSec <= startSec) {
        QMessageBox::critical(this, "Error", "Invalid time values!\nStart must be less than end and >= 0.");
        return;
    }

    // Создаем прогресс-диалог в главном потоке
   // QProgressDialog* progressDialog = new QProgressDialog("Processing audio...", "Cancel", 0, 100, this);
   // progressDialog->setWindowModality(Qt::WindowModal);
    //progressDialog->setMinimumDuration(0);

    // Атомарные переменные для обмена между потоками
    //std::atomic<bool> processingCanceled(false);    // для безопасного из разных потоков флага отмены обработки
    //std::atomic<int> currentProgress(0);    //  для отслеживания текущего прогресса выполнения задачи в многопоточной среде

    // Запускаем обработку в отдельном потоке
    QFuture<void> future = QtConcurrent::run([this, fileName, savePath, startSec, endSec]() {
        GstElement *pipeline = nullptr;
        GstElement *source = nullptr;
        GstElement *decode = nullptr;
        GstElement *convert = nullptr;
        GstElement *resample = nullptr;
        GstElement *sink = nullptr;
        GstCaps *raw_caps = nullptr;

        auto cleanup = [&]() {
            if (raw_caps) gst_caps_unref(raw_caps);
            if (pipeline) {
                gst_element_set_state(pipeline, GST_STATE_NULL);
                gst_object_unref(pipeline);
            }
        };

        try {
            // Инициализация конвейера
            pipeline = gst_pipeline_new("audio-remove");
            if (!pipeline) throw std::runtime_error("Failed to create pipeline");

            source = gst_element_factory_make("filesrc", "source");
            decode = gst_element_factory_make("decodebin", "decode");
            convert = gst_element_factory_make("audioconvert", "convert");
            resample = gst_element_factory_make("audioresample", "resample");
            sink = gst_element_factory_make("appsink", "sink");

            if (!source || !decode || !convert || !resample || !sink) {
                throw std::runtime_error("Failed to create pipeline elements");
            }

            g_object_set(source, "location", fileName.toUtf8().constData(), NULL);
            g_object_set(sink, "emit-signals", TRUE, "sync", FALSE, NULL);

            gst_bin_add_many(GST_BIN(pipeline), source, decode, convert, resample, sink, NULL);

            if (!gst_element_link(source, decode)) {
                throw std::runtime_error("Failed to link source -> decode");
            }

            g_signal_connect(decode, "pad-added", G_CALLBACK(+[](GstElement *src, GstPad *pad, gpointer data){
                                 GstElement *convert = GST_ELEMENT(data);
                                 GstPad *sinkpad = gst_element_get_static_pad(convert, "sink");
                                 if (sinkpad && !gst_pad_is_linked(sinkpad)) {
                                     gst_pad_link(pad, sinkpad);
                                 }
                                 if (sinkpad) gst_object_unref(sinkpad);
                             }), convert);

            if (!gst_element_link_many(convert, resample, sink, NULL)) {
                throw std::runtime_error("Failed to link convert -> resample -> sink");
            }

            // Запускаем пайплайн
            gst_element_set_state(pipeline, GST_STATE_PLAYING);

            GstSample *probe_sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND);
            if (!probe_sample) {
                throw std::runtime_error("Failed to get initial sample");
            }

            GstCaps *caps = gst_sample_get_caps(probe_sample);
            GstStructure *str = gst_caps_get_structure(caps, 0);
            int sample_rate = 96000, channels = 2;
            gst_structure_get_int(str, "rate", &sample_rate);
            gst_structure_get_int(str, "channels", &channels);
            const char *fmt = gst_structure_get_string(str, "format");
            if (!fmt) {
                gst_sample_unref(probe_sample);
                throw std::runtime_error("Failed to get format from caps");
            }

            gst_sample_unref(probe_sample);

            // Получаем длительность файла
            GstClockTime duration = GST_CLOCK_TIME_NONE;
            gint64 duration_ns = 0;
            if (gst_element_query_duration(pipeline, GST_FORMAT_TIME, &duration_ns)) {
                duration = static_cast<GstClockTime>(duration_ns);
            }

            // Определяем, где резать в файле
            GstClockTime remove_start = static_cast<GstClockTime>(startSec * GST_SECOND);
            GstClockTime remove_end = static_cast<GstClockTime>(endSec * GST_SECOND);

            if (duration != GST_CLOCK_TIME_NONE) {
                if (remove_end > duration) {
                    QMetaObject::invokeMethod(this, [this, duration]() {
                        QMessageBox::warning(
                            this,
                            "Warning",
                            QString("End time exceeds file duration. Adjusted to %1 sec")
                                .arg(static_cast<double>(duration) / GST_SECOND)
                            );
                    }, Qt::BlockingQueuedConnection);
                    remove_end = duration;
                }
                if (remove_start >= remove_end) {
                    throw std::runtime_error("Invalid time range after adjustment");
                }
            }

            // Создаем caps для выходного потока
            raw_caps = gst_caps_new_simple("audio/x-raw",
                                           "format", G_TYPE_STRING, fmt,
                                           "rate", G_TYPE_INT, sample_rate,
                                           "channels", G_TYPE_INT, channels,
                                           "layout", G_TYPE_STRING, "interleaved",
                                           NULL);

            // Собираем буферы
            std::vector<GstBuffer*> buffers;
            bool processing = true;

            while (processing) {

                GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
                if (!sample) {
                    processing = false;
                    continue;
                }

                GstBuffer *buf = gst_sample_get_buffer(sample);
                if (!buf) {
                    gst_sample_unref(sample);
                    continue;
                }

                GstClockTime buf_start = GST_BUFFER_PTS(buf);
                GstClockTime buf_end = buf_start + GST_BUFFER_DURATION(buf);

                // Фильтруем буферы по времени
                if (buf_end <= remove_start || buf_start >= remove_end) {
                    buffers.push_back(gst_buffer_copy(buf));
                }
                else if (buf_start < remove_start && buf_end > remove_start) {
                    guint64 buf_size = gst_buffer_get_size(buf);
                    guint64 copy_size = (remove_start - buf_start) * buf_size / GST_BUFFER_DURATION(buf);
                    if (copy_size > 0) {
                        GstBuffer *part = gst_buffer_copy_region(buf, GST_BUFFER_COPY_ALL, 0, copy_size);
                        if (part) buffers.push_back(part);
                    }
                }
                else if (buf_start < remove_end && buf_end > remove_end) {
                    guint64 buf_size = gst_buffer_get_size(buf);
                    guint64 copy_offset = (remove_end - buf_start) * buf_size / GST_BUFFER_DURATION(buf);
                    if (copy_offset < buf_size) {
                        GstBuffer *part = gst_buffer_copy_region(buf, GST_BUFFER_COPY_ALL,
                                                                 copy_offset, buf_size - copy_offset);
                        if (part) buffers.push_back(part);
                    }
                }

                gst_sample_unref(sample);
            }

            if (buffers.empty()) {
                throw std::runtime_error("No audio data collected");
            }

            // Сохраняем результат
            std::string ext = savePath.section('.', -1).toLower().toStdString();
            EncoderInfo enc_info = choose_encoder_by_extension(ext);

            GstElement *pipeline_out = gst_pipeline_new("output-pipeline");
            GstElement *appsrc = gst_element_factory_make("appsrc", "appsrc");
            GstElement *encoder = enc_info.encoder_name ?
                                      gst_element_factory_make(enc_info.encoder_name, "encoder") : nullptr;
            GstElement *muxer = enc_info.muxer_name ?
                                    gst_element_factory_make(enc_info.muxer_name, "muxer") : nullptr;
            GstElement *filesink = gst_element_factory_make("filesink", "filesink");
            GstElement *conv_out = gst_element_factory_make("audioconvert", "conv_out");
            GstElement *resample_out = gst_element_factory_make("audioresample", "resample_out");

            if (!pipeline_out || !appsrc || !conv_out || !resample_out || !filesink ||
                (enc_info.encoder_name && !encoder) || (enc_info.muxer_name && !muxer)) {
                throw std::runtime_error("Failed to create output pipeline elements");
            }

            g_object_set(filesink, "location", savePath.toUtf8().constData(), NULL);

            if (muxer) {
                gst_bin_add_many(GST_BIN(pipeline_out), appsrc, conv_out, resample_out,
                                 encoder, muxer, filesink, NULL);
                if (!gst_element_link_many(appsrc, conv_out, resample_out, encoder, muxer, filesink, NULL)) {
                    throw std::runtime_error("Failed to link output pipeline with muxer");
                }
            } else {
                gst_bin_add_many(GST_BIN(pipeline_out), appsrc, conv_out, resample_out,
                                 encoder, filesink, NULL);
                if (!gst_element_link_many(appsrc, conv_out, resample_out, encoder, filesink, NULL)) {
                    throw std::runtime_error("Failed to link output pipeline");
                }
            }

            g_object_set(appsrc, "caps", raw_caps, "format", GST_FORMAT_TIME, NULL);

            // Отправляем начальные события
            {
                GstEvent *caps_event = gst_event_new_caps(raw_caps);
                gst_element_send_event(appsrc, caps_event);

                GstEvent *stream_start = gst_event_new_stream_start("audio");
                gst_element_send_event(appsrc, stream_start);

                GstSegment segment;
                gst_segment_init(&segment, GST_FORMAT_TIME);
                GstEvent *segment_event = gst_event_new_segment(&segment);
                gst_element_send_event(appsrc, segment_event);
            }

            // Запускаем пайплайн
            gst_element_set_state(pipeline_out, GST_STATE_PLAYING);

            // Отправляем буферы
            for (auto &buf : buffers) {

                GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buf);
                if (ret != GST_FLOW_OK) {
                    qWarning() << "Failed to push buffer to appsrc:" << ret;
                    break;
                }
            }

            gst_app_src_end_of_stream(GST_APP_SRC(appsrc));

            // Ждем завершения
            GstBus *bus = gst_element_get_bus(pipeline_out);
            GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
                                                         static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));

            if (msg) {
                if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                    GError *err = nullptr;
                    gchar *dbg = nullptr;
                    gst_message_parse_error(msg, &err, &dbg);
                    QString errorMsg = QString("Error saving file: %1").arg(err->message);
                    g_error_free(err);
                    g_free(dbg);
                    throw std::runtime_error(errorMsg.toStdString());
                }
                gst_message_unref(msg);
            }

            gst_object_unref(bus);
            gst_element_set_state(pipeline_out, GST_STATE_NULL);
            gst_object_unref(pipeline_out);

        } catch (const std::exception &e) {
            QString error = QString("Error: %1").arg(e.what());
            QMetaObject::invokeMethod(this, [this, error]() {
                QMessageBox::critical(this, "Error", error);
            }, Qt::BlockingQueuedConnection);
            cleanup();
        }
    });

    // Обработчик завершения операции
    QFutureWatcher<void>* watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, [this]() {
        QMessageBox::information(this, "Success", "Operation completed");
    });
    connect(watcher, &QFutureWatcher<void>::finished, watcher, &QObject::deleteLater);
    watcher->setFuture(future);
    futureWatchers.append(watcher);

    // Окошко для отслеживания процесса создания нового аудиофайла
    // QPointer<QProgressDialog> progressDialogPtr = new QProgressDialog("Processing audio...", "Cancel", 0, 100, this);
    // progressDialogPtr->setWindowModality(Qt::WindowModal);
    // progressDialogPtr->setMinimumDuration(0);
    // progressDialogPtr->setAttribute(Qt::WA_DeleteOnClose);
    // //bool closed = false;

    // QTimer* progressTimer = new QTimer(this);
    // connect(progressTimer, &QTimer::timeout, [progressDialogPtr, &currentProgress]() {
    //     QMetaObject::invokeMethod(progressDialogPtr.data(), [progressDialogPtr, &currentProgress]() {
    //         if (progressDialogPtr && !progressDialogPtr->wasCanceled()) {
    //             progressDialogPtr->setValue(currentProgress.load());
    //         }
    //     });
    // });

    // connect(progressDialogPtr, &QProgressDialog::canceled, [&processingCanceled]() {
    //     processingCanceled.store(true);
    // });

    // progressTimer->start(100);

    // QFutureWatcher<void>* watcher = new QFutureWatcher<void>(this);
    // connect(watcher, &QFutureWatcher<void>::finished, [progressDialogPtr, progressTimer, watcher]() mutable {
    //     progressTimer->stop();

    //     if (progressDialogPtr) {
    //         progressDialogPtr->close(); // Закроет и удалит благодаря WA_DeleteOnClose
    //     }

    //     progressTimer->deleteLater();
    //     watcher->deleteLater();
    // });
    // watcher->setFuture(future);
}

void MainWindow::on_action_3_triggered()
{   // функция микширования двух аудиофайлов. Вставка второго аудиофайла в любую позицию первого
    QString file1 = QFileDialog::getOpenFileName(this, "Select First Audio File");
    QString file2 = QFileDialog::getOpenFileName(this, "Select Second Audio File");
    if (file1.isEmpty() || file2.isEmpty()) return;

    bool ok;
    double insert_sec = QInputDialog::getDouble(this, "Insert Position", "Insert at (seconds):", 0, 0, 10000, 2, &ok);
    if (!ok) return;

    QString outfile = QFileDialog::getSaveFileName(this, "Save As", "", "*.wav;; *.raw;; *.aiff;; *.aifc;; *.au;; *.mp3;; *.flac;; *.ogg;; *.opus;; *.gsm;; *.spx;; *.wma;; *.m4a");
    if (outfile.isEmpty()) return;

    gchar *uri1 = gst_filename_to_uri(file1.toUtf8().constData(), nullptr);
    gchar *uri2 = gst_filename_to_uri(file2.toUtf8().constData(), nullptr);


    // Запускаем обработку в отдельном потоке
    QFuture<void> future = QtConcurrent::run([this, file1, file2, outfile, insert_sec, uri1, uri2]() {

    GstElement *pipeline = gst_pipeline_new("insert-pipeline");

    // Source 1 - перед вставкой
    GstElement *src1_before = gst_element_factory_make("uridecodebin", "src1_before");
    g_object_set(src1_before, "uri", uri1, nullptr);
    GstElement *conv1 = gst_element_factory_make("audioconvert", nullptr);
    GstElement *resample1 = gst_element_factory_make("audioresample", nullptr);
    GstElement *queue1 = gst_element_factory_make("queue", nullptr);

    // Source 2 - файл для вставки
    GstElement *src2 = gst_element_factory_make("uridecodebin", "src2");
    g_object_set(src2, "uri", uri2, nullptr);
    GstElement *conv2 = gst_element_factory_make("audioconvert", nullptr);
    GstElement *resample2 = gst_element_factory_make("audioresample", nullptr);
    GstElement *queue2 = gst_element_factory_make("queue", nullptr);

    // Source 1 снова - после вставки
    GstElement *src1_after = gst_element_factory_make("uridecodebin", "src1_after");
    g_object_set(src1_after, "uri", uri1, nullptr);
    GstElement *conv3 = gst_element_factory_make("audioconvert", nullptr);
    GstElement *resample3 = gst_element_factory_make("audioresample", nullptr);
    GstElement *queue3 = gst_element_factory_make("queue", nullptr);

    // Concat
    GstElement *concat = gst_element_factory_make("concat", nullptr);

    // Encoder и sink
    GstElement *filter = gst_element_factory_make("capsfilter", nullptr);
    GstElement *encoder = nullptr;
    GstElement *muxer = nullptr;
    GstElement *sink = gst_element_factory_make("filesink", nullptr);
    g_object_set(sink, "location", outfile.toUtf8().constData(), nullptr);

    // Настройка кодека и муксеры в зависимости от формата
    QString ext = QFileInfo(outfile).suffix().toLower();
    if (ext == "mp3") {
        encoder = gst_element_factory_make("lamemp3enc", nullptr);
        muxer = nullptr;  // MP3 не требует муксер
        g_object_set(encoder, "quality", 2, nullptr);
    } else if (ext == "wav") {
        encoder = gst_element_factory_make("wavenc", nullptr);
        muxer = gst_element_factory_make("identity", nullptr);
    } else if (ext == "ogg") {
        encoder = gst_element_factory_make("vorbisenc", nullptr);
        muxer = gst_element_factory_make("oggmux", nullptr);
    } else if (ext == "flac") {
        encoder = gst_element_factory_make("flacenc", nullptr);
        muxer = gst_element_factory_make("identity", nullptr);
    } else if (ext == "opus") {
        encoder = gst_element_factory_make("opusenc", nullptr);
        muxer = gst_element_factory_make("oggmux", nullptr);
    } else if (ext == "m4a") {
        encoder = gst_element_factory_make("faac", nullptr);
        muxer = gst_element_factory_make("mp4mux", nullptr);
    } else if (ext == "wma") {
        encoder = gst_element_factory_make("wmav2enc", nullptr);
        muxer = gst_element_factory_make("asfmux", nullptr);
    } else if (ext == "aiff" || ext == "aifc") {
        encoder = gst_element_factory_make("aiffenc", nullptr);
        muxer = gst_element_factory_make("identity", nullptr);
    } else if (ext == "au") {
        encoder = gst_element_factory_make("avenc_au", nullptr);
        muxer = gst_element_factory_make("identity", nullptr);
    } else if (ext == "gsm") {
        encoder = gst_element_factory_make("avenc_gsm", nullptr);
        muxer = gst_element_factory_make("identity", nullptr);
    } else if (ext == "spx") {
        encoder = gst_element_factory_make("speexenc", nullptr);
        muxer = gst_element_factory_make("oggmux", nullptr);
    } else {
        // По умолчанию используем WAV
        encoder = gst_element_factory_make("wavenc", nullptr);
        muxer = gst_element_factory_make("identity", nullptr);
    }

    // Caps
    GstCaps *caps = gst_caps_new_simple("audio/x-raw",
                                        "channels", G_TYPE_INT, 8,
                                        "rate", G_TYPE_INT, 96000, nullptr);
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    // Добавляем элементы в пайплайн
    if (muxer){
    gst_bin_add_many(GST_BIN(pipeline),
                     src1_before, conv1, resample1, queue1,
                     src2, conv2, resample2, queue2,
                     src1_after, conv3, resample3, queue3,
                         concat, filter, encoder, muxer, sink, nullptr);
        // Линкуем части
        gst_element_link_many(queue1, concat, nullptr);
        gst_element_link_many(queue2, concat, nullptr);
        gst_element_link_many(queue3, concat, nullptr);
        gst_element_link_many(concat, filter, encoder, muxer, sink, nullptr);
    }
    else{
        gst_bin_add_many(GST_BIN(pipeline),
                         src1_before, conv1, resample1, queue1,
                         src2, conv2, resample2, queue2,
                         src1_after, conv3, resample3, queue3,
                         concat, filter, encoder, sink, nullptr);
        // Линкуем части
        gst_element_link_many(queue1, concat, nullptr);
        gst_element_link_many(queue2, concat, nullptr);
        gst_element_link_many(queue3, concat, nullptr);
        gst_element_link_many(concat, filter, encoder, sink, nullptr);
    }



    // Коллбэк функции
    g_signal_connect(src1_before, "pad-added", G_CALLBACK(onPadAdded), conv1);
    g_signal_connect(src2,        "pad-added", G_CALLBACK(onPadAdded), conv2);
    g_signal_connect(src1_after,  "pad-added", G_CALLBACK(onPadAdded), conv3);

    // Линкуем конвертеры
    gst_element_link_many(conv1, resample1, queue1, nullptr);
    gst_element_link_many(conv2, resample2, queue2, nullptr);
    gst_element_link_many(conv3, resample3, queue3, nullptr);

    // Seek
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    QThread::msleep(500); // Ждём добавления падов

    // Seek: перед вставкой
    gst_element_seek(src1_before, 1.0, GST_FORMAT_TIME,
                     GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
                     GST_SEEK_TYPE_SET, 0,
                     GST_SEEK_TYPE_SET, (gint64)(insert_sec * GST_SECOND));

    // Seek: после вставки
    gst_element_seek(src1_after, 1.0, GST_FORMAT_TIME,
                     GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
                     GST_SEEK_TYPE_SET, (gint64)(insert_sec * GST_SECOND),
                     GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);

    // Шина сообщений
    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, [](GstBus *, GstMessage *msg, gpointer data) -> gboolean {
        GMainLoop *loop = static_cast<GMainLoop*>(data);
        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err;
            gchar *dbg;
            gst_message_parse_error(msg, &err, &dbg);
            qCritical() << "Error:" << err->message;
            g_error_free(err);
            g_free(dbg);
            g_main_loop_quit(loop);
            break;
        }
        case GST_MESSAGE_EOS:
            g_main_loop_quit(loop);
            break;
        default:
            break;
        }
        return TRUE;
    }, loop);

    // Запуск пайплайна
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_main_loop_run(loop);

    // Чистим память
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    gst_object_unref(bus);
    g_main_loop_unref(loop);
    g_free(uri1);
    g_free(uri2);  
    });

    // Обработчик завершения операции
    QFutureWatcher<void>* watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, [this]() {
        QMessageBox::information(this, "Success", "Operation completed");
    });
    connect(watcher, &QFutureWatcher<void>::finished, watcher, &QObject::deleteLater);
    watcher->setFuture(future);
    futureWatchers.append(watcher);

}

void MainWindow::on_action_4_triggered()
{   // функция конвертации из одного аудиоформата в другой

    QString inputPath = QFileDialog::getOpenFileName(this, "Select Audio File");
    if (inputPath.isEmpty()) return;

    // Получаем информацию о входном файле
    QFileInfo inputFileInfo(inputPath);
    QString baseName = inputFileInfo.completeBaseName(); // Имя файла без расширения
    QString inputDir = inputFileInfo.path(); // Директория файла

    // Диалог сохранения с предустановленным именем
    QString outputPath = QFileDialog::getSaveFileName(
        this,
        "Save As",
        inputDir + "/" + baseName, // Начальное имя файла
        "Audio Files (*.wav;; *.raw;; *.aiff;; *.aifc;; *.au;; *.mp3;; *.flac;; *.ogg;; *.opus;; *.gsm;; *.spx;; *.wma;; *.m4a"
        );

    if (outputPath.isEmpty()) return;

    // Если пользователь не указал расширение, добавляем первое из фильтра (.wav)
    if (QFileInfo(outputPath).suffix().isEmpty()) {
        outputPath += ".wav";
    }

    // Определяем encoder на основе расширения файла
    QString encoder;
    if (outputPath.endsWith(".wav", Qt::CaseInsensitive)) {
        encoder = "wavenc";
    } else if (outputPath.endsWith(".raw", Qt::CaseInsensitive)) {
        encoder = "audio/x-raw, format=(string)S16LE ! filesink"; // RAW PCM data
    } else if (outputPath.endsWith(".aiff", Qt::CaseInsensitive) ||
               outputPath.endsWith(".aifc", Qt::CaseInsensitive)) {
        encoder = "aiffenc";
    } else if (outputPath.endsWith(".au", Qt::CaseInsensitive)) {
        encoder = "avmux_au";
    } else if (outputPath.endsWith(".mp3", Qt::CaseInsensitive)) {
        encoder = "lamemp3enc ! id3v2mux";
    } else if (outputPath.endsWith(".flac", Qt::CaseInsensitive)) {
        encoder = "flacenc";
    } else if (outputPath.endsWith(".ogg", Qt::CaseInsensitive)) {
        encoder = "vorbisenc ! oggmux";
    } else if (outputPath.endsWith(".opus", Qt::CaseInsensitive)) {
        encoder = "opusenc ! oggmux";
    } else if (outputPath.endsWith(".gsm", Qt::CaseInsensitive)) {
        encoder = "gsmenc";
    } else if (outputPath.endsWith(".spx", Qt::CaseInsensitive)) {
        encoder = "speexenc ! oggmux";
    } else if (outputPath.endsWith(".wma", Qt::CaseInsensitive)) {
        encoder = "avenc_wmav2";
    } else if (outputPath.endsWith(".m4a", Qt::CaseInsensitive)) {
        encoder = "avenc_aac ! hlsmediaplaylist";
    } else {
        qWarning() << "Unsupported output format, defaulting to WAV";
        encoder = "wavenc";
    }

    // Запуск в отдельном потоке
    QFuture<void> future = QtConcurrent::run([this, inputPath, outputPath, encoder]() {
    // Создаем pipeline
    QString pipelineStr = QString("filesrc location=\"%1\" ! decodebin ! audioconvert ! audioresample ! %2 ! filesink location=\"%3\"")
                              .arg(inputPath).arg(encoder).arg(outputPath);

    GstElement *pipeline = gst_parse_launch(pipelineStr.toUtf8().constData(), nullptr);
    if (!pipeline) {
        qCritical() << "Failed to create pipeline";
        gst_deinit();
        return;
    }

    // Запускаем pipeline
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        qCritical() << "Failed to start pipeline";
        gst_object_unref(pipeline);
        gst_deinit();
        return;
    }

    // Ожидаем завершения
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
                                                 static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

    if (msg != nullptr) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);
            qCritical() << "Error:" << err->message << debug;
            g_error_free(err);
            g_free(debug);
        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            qInfo() << "Conversion completed successfully";
            // Показываем QMessageBox в главном потоке
            QMetaObject::invokeMethod(this, [this]() {
                QMessageBox::information(this, "Success", "Conversion completed successfully");
            }, Qt::BlockingQueuedConnection);
        }
        gst_message_unref(msg);
    }

    // Очистка
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    return void();

    });

    QFutureWatcher<void>* watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher]() {
        watcher->deleteLater();
        futureWatchers.removeAll(watcher);
    });
    watcher->setFuture(future);
    futureWatchers.append(watcher);
}


void MainWindow::on_onSave_clicked()
{   // функция сохранения воспроизводимого аудиофайла с новыми настройками эквалайзера
    if (fileName.isEmpty()) {
        QMessageBox::warning(this, "Ошибка", "Не выбран файл для обработки");
        return;
    }

    // Получаем расширение исходного файла
    QString originalExtension = QFileInfo(fileName).suffix().toLower();
    QString filter;

    // Устанавливаем фильтр для сохранения только в исходном формате
    if (originalExtension == "mp3") {
        filter = "MP3 (*.mp3)";
    } else if (originalExtension == "flac") {
        filter = "FLAC (*.flac)";
    } else if (originalExtension == "wav") {
        filter = "WAV (*.wav)";
    } else if (originalExtension == "ogg") {
        filter = "OGG (*.ogg)";
    } else if (originalExtension == "aac") {
        filter = "AAC (*.aac)";
    } else {
        QMessageBox::warning(this, "Ошибка", "Неподдерживаемый формат исходного файла: " + originalExtension);
        return;
    }

    // Выбираем файл для сохранения (только с таким же расширением)
    QString outputFile = QFileDialog::getSaveFileName(
        this,
        "Сохранить аудиофайл",
        QDir::homePath() + "/converted_audio/" + QFileInfo(fileName).completeBaseName() + "_equalized." + originalExtension,
        filter
        );

    if (outputFile.isEmpty()) return;

    // Проверяем, что выбранное расширение совпадает с исходным
    if (QFileInfo(outputFile).suffix().toLower() != originalExtension) {
        QMessageBox::warning(this, "Ошибка", "Файл должен сохраняться в том же формате, что и исходный (" + originalExtension + ")");
        return;
    }

    // Запуск в отдельном потоке
    QFuture<void> future = QtConcurrent::run([this, outputFile, originalExtension]() {

    // Создаем отдельный pipeline для записи
    GstElement *recordPipeline = gst_pipeline_new("record-pipeline");
    GstElement *fileSrc = gst_element_factory_make("filesrc", "file-source");
    GstElement *decodeBin = gst_element_factory_make("decodebin", "decoder");
    GstElement *audioConvert = gst_element_factory_make("audioconvert", "convert");
    GstElement *audioResample = gst_element_factory_make("audioresample", "resample");
    GstElement *volumeElem = gst_element_factory_make("volume", "volume");
    GstElement *equalizerElem = gst_element_factory_make("equalizer-10bands", "equalizer");
    GstElement *encoder = nullptr;
    //GstElement *muxer = nullptr;
    GstElement *fileSink = gst_element_factory_make("filesink", "file-output");

    // Создаем encoder в соответствии с исходным форматом
    if (originalExtension == "mp3") {
        encoder = gst_element_factory_make("lamemp3enc", "mp3-encoder");
        g_object_set(encoder, "target", 1, "bitrate", 192, NULL);  // 192 kbps
    }
    else if (originalExtension == "flac") {
        encoder = gst_element_factory_make("flacenc", "flac-encoder");
    }
    else if (originalExtension == "wav") {
        encoder = gst_element_factory_make("wavenc", "wav-encoder");
    }
    else if (originalExtension == "ogg") {
        encoder = gst_element_factory_make("vorbisenc", "vorbis-encoder");
        //muxer = gst_element_factory_make("oggmux", "ogg-muxer");
    }
    else if (originalExtension == "aac") {
        encoder = gst_element_factory_make("faac", "aac-encoder");
        //muxer = gst_element_factory_make("mp4mux", "mp4-muxer");
    }

    // Собираем все слайдеры эквалайзера
    QList<QSlider*> eqSliders = {
        ui->eqBand0, ui->eqBand1, ui->eqBand2, ui->eqBand3, ui->eqBand4,
        ui->eqBand5, ui->eqBand6, ui->eqBand7, ui->eqBand8, ui->eqBand9
    };

    // Устанавливаем параметры эквалайзера (те же, что в UI)
    for (int i = 0; i < 10; i++) {
        QByteArray bandProp = QString("band%1").arg(i).toUtf8();
        double gain = eqSliders[i]->value() / 10.0;  // Предполагаем, что слайдеры именуются eqSliders[0..9]
        g_object_set(equalizerElem, bandProp.constData(), gain, NULL);
    }

    // Настраиваем выходной файл
    g_object_set(fileSink, "location", outputFile.toUtf8().constData(), NULL);

    // Собираем pipeline
    gst_bin_add_many(
        GST_BIN(recordPipeline),
        fileSrc, decodeBin, audioConvert, audioResample,
        volumeElem, equalizerElem, encoder, fileSink,
        NULL
        );

    // Пробуем линковать элементы
    if (!gst_element_link_many(fileSrc, decodeBin, NULL)) {
        qWarning("Failed to link src -> decoder");
    }

    // Динамическая линковка decodebin -> audioconvert
    g_signal_connect(decodeBin, "pad-added", G_CALLBACK(onPadAdded), audioConvert);

    // Линкуем остальные элементы
    if (!gst_element_link_many(
            audioConvert, audioResample, volumeElem, equalizerElem, encoder, fileSink, NULL
            )) {
        qWarning("Failed to link audio processing elements");
    }

    // Устанавливаем входной файл
    g_object_set(fileSrc, "location", fileName.toUtf8().constData(), NULL);

    // Запускаем запись
    gst_element_set_state(recordPipeline, GST_STATE_PLAYING);

    // Ждем завершения
    GstBus *bus = gst_element_get_bus(recordPipeline);
    GstMessageType mask = static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, mask);

    // Обработка результата
    if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            QMetaObject::invokeMethod(this, [this]() {
                QMessageBox::information(this, "Успех", "Файл успешно сохранен!");
            }, Qt::BlockingQueuedConnection);
            //QMessageBox::information(this, "Успех", "Файл успешно сохранен!");
        } else {
            GError *err = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(msg, &err, &debug);
            QMetaObject::invokeMethod(this, [this, err]() {
                QMessageBox::critical(this, "Error", QString("Ошибка записи: %1").arg(err->message));
            }, Qt::BlockingQueuedConnection);
            //QMessageBox::critical(this, "Ошибка", QString("Ошибка записи: %1").arg(err->message));
            g_error_free(err);
            g_free(debug);
        }
        gst_message_unref(msg);
    }

    // Очистка
    gst_element_set_state(recordPipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(recordPipeline);

    });

    QFutureWatcher<void>* watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher]() {
        watcher->deleteLater();
        futureWatchers.removeAll(watcher);
    });
    watcher->setFuture(future);
    futureWatchers.append(watcher);

}

