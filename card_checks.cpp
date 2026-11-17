#include "card_checks.h"
#include "gvfg_capture.h"
#include "session.h"
#include <QDir>
#include <QPainter>
#include <QSaveFile>
#include "gigabyte/gigabyte_device.h"

#include <QElapsedTimer>
#include <QImage>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
auto captureDevices()
{
    auto devices = gvfg::internal::enumerate_gigabyte_devices();
    if (devices.size() > GVFG_MAX_DEVICES) devices.resize(GVFG_MAX_DEVICES);
    gvfg_device_info_t sdkDevices[GVFG_MAX_DEVICES]{};
    if (gvfg_enumerate_devices(sdkDevices, GVFG_MAX_DEVICES) != int(devices.size())) {
        devices.clear();
        return devices;
    }
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto suffix = QByteArray(" #") + QByteArray::number(i + 1);
        const auto name = QString::fromStdWString(devices[i].friendly_name).toUtf8();
        if (sdkDevices[i].name != name.left(qsizetype(sizeof(sdkDevices[i].name)) - 1 - suffix.size()) + suffix) {
            devices.clear();
            break;
        }
    }
    return devices;
}

gvfg_status_t openSelectedChannel(gvfg_handle handle, const QString &path, int channel)
{
    const auto devices = captureDevices();
    for (size_t i = 0; i < devices.size(); ++i) {
        if (QString::fromStdWString(devices[i].interface_path).compare(path, Qt::CaseInsensitive) == 0)
            return gvfg_open_channel(handle, int(i), channel);
    }
    return GVFG_ENODEV;
}

struct CheckResult {
    bool passed;
    QString detail;
};

QString sdkError(gvfg_status_t status)
{
    return QString("%1 (%2)").arg(QString::fromUtf8(gvfg_strerror(status))).arg(status);
}

struct Channel
{
    gvfg_handle handle = nullptr;
    int index = 0;
    std::function<void(const QString &)> log;
    ~Channel() { if (handle) gvfg_destroy(handle); }

    QString error(const QString &api, gvfg_status_t status) const
    {
        QString message = api + " · " + sdkError(status);
        char detail[1024]{};
        if (handle && gvfg_get_channel_last_sdk_error_detail(handle, index, detail, sizeof(detail)) == GVFG_OK
            && detail[0]) message += " · " + QString::fromUtf8(detail);
        log("錯誤 · " + message);
        return message;
    }
};

CheckResult readSignal(const Channel &channel, gvfg_signal_status_t &signal,
                       int timeout, const std::atomic_bool &stop)
{
    QElapsedTimer timer;
    timer.start();
    do {
        const auto status = gvfg_get_channel_signal_status(channel.handle, channel.index, &signal);
        if (status != GVFG_OK) return {false, channel.error("查詢訊號狀態", status)};
        if (signal.connected) return {true, "輸入已鎖定"};
        QThread::msleep(100);
    } while (!stop && timer.elapsed() < timeout);
    if (!stop) channel.log("警告 · 等待輸入鎖定逾時");
    return {false, "等待輸入鎖定逾時"};
}

CheckResult checkFormat(const gvfg_signal_status_t &signal, const QJsonObject &settings)
{
    const QString interface = signal.video_interface == GVFG_INPUT_INTERFACE_SDI ? "SDI"
        : signal.video_interface == GVFG_INPUT_INTERFACE_HDMI ? "HDMI" : "未知";
    const QString expected = settings.value("interface").toString();
    const int width = settings.value("width").toInt(), height = settings.value("height").toInt();
    const int depth = settings.value("depth").toInt();
    return { (expected.isEmpty() || expected == interface)
          && (!width || signal.width == width) && (!height || signal.height == height)
          && (!depth || signal.bit_depth == depth),
        QString("%1 %2×%3，%4-bit").arg(interface).arg(signal.width).arg(signal.height).arg(signal.bit_depth) };
}

struct Capture
{
    int frames = 0;
    int audioBlocks = 0;
    double fps = 0;
    gvfg_audio_format_t pcm{};
    int unplugEvents = 0;
    QString videoError, audioError, eventError;
};

struct ImageSamples
{
    QImage reference, alignedReference;
    QImage blackEvidence, freezeEvidence, colorEvidence, referenceEvidence;
    bool saveEvidence = false, monitorBlack = false, monitorColor = false, monitorReference = false;
    double colorLimit = 15, referenceLimit = 15;
    int freezeLimit = 1000;
    qint64 freezeFrom = 0, freezeTo = 0;
    QByteArray previous;
    bool monitorFreeze = false;
    int previousWidth = 0, previousHeight = 0, previousFormat = 0;
    int count = 0, black = 0;
    qint64 freezeMs = 0, lastChange = 0;
    double maxColor = 0, maxDifference = 0;
    QString error;

    static QImage preview(const gvfg_frame_t &frame)
    {
        const QSize size = QSize(frame.width, frame.height).scaled(960, 540, Qt::KeepAspectRatio);
        QImage image(size.boundedTo(QSize(frame.width, frame.height)), QImage::Format_RGB32);
        if (image.isNull()) return {};
        const bool ten = frame.pixel_format == GVFG_PIXFMT_Y210;
        for (int y = 0; y < image.height(); ++y) {
            const auto *row = static_cast<const uchar *>(frame.data)
                + qsizetype(y * qint64(frame.height) / image.height()) * frame.row_stride_bytes;
            auto *dst = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const int column = int(x * qint64(frame.width) / image.width());
                const auto *pair = row + qsizetype(column / 2) * (ten ? 8 : 4);
                const auto read = [&](int i) { return ten ? pair[2 * i + 1] : pair[i]; };
                const double yy = (read(column % 2 ? 2 : 0) - 16) * (255.0 / 219);
                const double u = (read(1) - 128) * (255.0 / 224), v = (read(3) - 128) * (255.0 / 224);
                dst[x] = qRgb(qBound(0, int(yy + 1.5748 * v), 255),
                    qBound(0, int(yy - .1873 * u - .4681 * v), 255), qBound(0, int(yy + 1.8556 * u), 255));
            }
        }
        return image;
    }

    static QImage pairImages(const QImage &left, const QImage &right)
    {
        if (left.isNull() || right.isNull()) return {};
        QImage pair(left.width() + right.width(), left.height(), QImage::Format_RGB32);
        QPainter painter(&pair);
        painter.drawImage(0, 0, left);
        painter.drawImage(left.width(), 0, right);
        return pair;
    }

    QImage comparison(const gvfg_frame_t &frame, const QRect &block) const
    {
        const auto actual = preview(frame);
        if (actual.isNull()) return {};
        auto pair = pairImages(actual, alignedReference.scaled(actual.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        if (pair.isNull()) return {};
        QPainter painter(&pair);
        painter.setPen(QPen(Qt::red, 2));
        const double sx = double(actual.width()) / frame.width, sy = double(actual.height()) / frame.height;
        const QRectF mark(block.x() * sx, block.y() * sy, block.width() * sx, block.height() * sy);
        painter.drawRect(mark);
        painter.drawRect(mark.translated(actual.width(), 0));
        return pair;
    }

    void sample(const gvfg_frame_t &frame, qint64 ms)
    {
        constexpr int grid = 48;
        const bool ten = frame.pixel_format == GVFG_PIXFMT_Y210;
        const int pairBytes = ten ? 8 : 4;
        if ((frame.pixel_format != GVFG_PIXFMT_YUY2 && !ten) || !frame.data
            || frame.width < 2 || frame.width % 2 || frame.height < 1
            || frame.row_stride_bytes < qint64(frame.width / 2) * pairBytes
            || frame.data_size < quint64(frame.row_stride_bytes) * frame.height) {
            error = "無法分析影格格式或大小";
            return;
        }
        if (monitorFreeze) {
            const qsizetype rowBytes = qsizetype(frame.width / 2) * pairBytes;
            const auto *data = static_cast<const char *>(frame.data);
            bool changed = previousWidth != frame.width || previousHeight != frame.height
                || previousFormat != frame.pixel_format;
            for (int y = 0; !changed && y < frame.height; ++y) {
                const auto *row = data + qsizetype(y) * frame.row_stride_bytes;
                const auto *prior = previous.constData() + qsizetype(y) * rowBytes;
                changed = std::memcmp(prior, row, rowBytes) != 0;
            }
            previousWidth = frame.width;
            previousHeight = frame.height;
            previousFormat = frame.pixel_format;
            if (count) {
                const qint64 span = ms - lastChange;
                if (saveEvidence && !changed && span >= freezeLimit && freezeEvidence.isNull()) {
                    auto first = frame;
                    first.data = previous.constData();
                    first.row_stride_bytes = int(rowBytes);
                    freezeEvidence = pairImages(preview(first), preview(frame));
                    freezeFrom = lastChange;
                    freezeTo = ms;
                }
                freezeMs = qMax(freezeMs, span);
            }
            if (changed) {
                lastChange = ms;
                previous.resize(rowBytes * frame.height);
                for (int y = 0; y < frame.height; ++y)
                    std::memcpy(previous.data() + qsizetype(y) * rowBytes,
                                data + qsizetype(y) * frame.row_stride_bytes, rowBytes);
            }
        }
        const auto read = [ten](const uchar *pair, int component) {
            return ten ? int(pair[component * 2 + 1]) : int(pair[component]);
        };
        const auto rowAt = [&](int y) {
            return static_cast<const uchar *>(frame.data) + qsizetype(y) * frame.row_stride_bytes;
        };
        int dark = 0;
        for (int y = 0; y < grid; ++y) {
            const auto *row = rowAt(int(qint64(frame.height) * (2 * y + 1) / (2 * grid)));
            for (int x = 0; x < grid; ++x) {
                const int column = int(qint64(frame.width) * (2 * x + 1) / (2 * grid));
                const auto *pair = row + qsizetype(column / 2) * pairBytes;
                dark += read(pair, column % 2 ? 2 : 0) <= 24
                    && std::abs(read(pair, 1) - 128) <= 8 && std::abs(read(pair, 3) - 128) <= 8;
            }
        }
        if (dark >= grid * grid * .99) {
            ++black;
            if (saveEvidence && monitorBlack && blackEvidence.isNull()) blackEvidence = preview(frame);
        }
        ++count;
        if (reference.isNull()) return;
        if (alignedReference.size() != QSize(frame.width, frame.height)) {
            alignedReference = reference.scaled(frame.width, frame.height, Qt::IgnoreAspectRatio,
                Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
        }
        if (alignedReference.isNull()) {
            error = "無法將參考圖片調整為擷取解析度";
            return;
        }

        constexpr int block = 16;
        QRect colorBlock, differenceBlock;
        for (int top = 0; top < frame.height; top += block) {
            const int bottom = qMin(top + block, frame.height);
            for (int left = 0; left < frame.width; left += block) {
                const int right = qMin(left + block, frame.width);
                double color = 0, difference = 0;
                for (int y = top; y < bottom; ++y) {
                    const auto *row = rowAt(y);
                    const auto *expected = reinterpret_cast<const QRgb *>(alignedReference.constScanLine(y));
                    for (int x = left; x < right; ++x) {
                        const auto *pair = row + qsizetype(x / 2) * pairBytes;
                        const double yy = (read(pair, x % 2 ? 2 : 0) - 16) * (255.0 / 219);
                        const double u = (read(pair, 1) - 128) * (255.0 / 224);
                        const double v = (read(pair, 3) - 128) * (255.0 / 224);
                        const double r = qBound(0.0, yy + 1.5748 * v, 255.0) - qRed(expected[x]);
                        const double g = qBound(0.0, yy - .1873 * u - .4681 * v, 255.0) - qGreen(expected[x]);
                        const double b = qBound(0.0, yy + 1.8556 * u, 255.0) - qBlue(expected[x]);
                        difference += std::abs(r) + std::abs(g) + std::abs(b);
                        color += std::abs(r - g) + std::abs(b - g);
                    }
                }
                const int pixels = (right - left) * (bottom - top);
                if (const double score = color / (2 * pixels); score > maxColor) {
                    maxColor = score;
                    colorBlock = QRect(left, top, right - left, bottom - top);
                }
                if (const double score = difference / (3 * pixels); score > maxDifference) {
                    maxDifference = score;
                    differenceBlock = QRect(left, top, right - left, bottom - top);
                }
            }
        }
        if (saveEvidence && monitorColor && !colorBlock.isNull() && maxColor > colorLimit)
            colorEvidence = comparison(frame, colorBlock);
        if (saveEvidence && monitorReference && !differenceBlock.isNull() && maxDifference > referenceLimit)
            referenceEvidence = comparison(frame, differenceBlock);
    }
};

CheckResult checkContinuity(const Capture &capture)
{
    if (capture.unplugEvents)
        return {false, QString("收到 %1 次訊號中斷事件（Unplug）").arg(capture.unplugEvents)};
    if (!capture.eventError.isEmpty()) return {false, capture.eventError};
    return {true, "取樣期間未收到訊號中斷事件"};
}

Capture measure(const Channel &channel, bool video, bool audio, int duration,
                const std::atomic_bool &stop, int expectedDepth, bool monitorContinuity, ImageSamples *images)
{
    Capture result;
    QElapsedTimer timer;
    timer.start();
    bool pollEvents = true;
    while (!stop && timer.elapsed() < duration) {
        for (int i = 0; pollEvents && i < 32; ++i) {
            gvfg_event_type_t event{};
            const auto status = gvfg_poll_channel_event(channel.handle, channel.index, &event, 0);
            if (status == GVFG_ETIMEOUT) break;
            if (status != GVFG_OK) {
                result.eventError = channel.error("讀取 SDK 事件", status);
                pollEvents = false;
                break;
            }
            if (event == GVFG_EVENT_VIDEO_INPUT_UNPLUG) ++result.unplugEvents;
            const QString message = event == GVFG_EVENT_VIDEO_FORMAT_CHANGED ? "事件 · 影像格式變更"
                : event == GVFG_EVENT_VIDEO_INPUT_PLUGIN ? "事件 · 輸入訊號接入"
                : event == GVFG_EVENT_VIDEO_INPUT_UNPLUG ? "警告／事件 · 輸入訊號中斷" : "事件 · 未知事件";
            channel.log(QString("%1（%2）").arg(message).arg(int(event)));
        }
        bool received = false;
        if (video && result.videoError.isEmpty()) {
            gvfg_frame_t frame{};
            const auto status = gvfg_read_channel_frame(channel.handle, channel.index, &frame, 0);
            if (status == GVFG_OK) {
                const bool valid = frame.data && frame.data_size && frame.width > 0 && frame.height > 0;
                const int expectedFormat = expectedDepth == 10 ? GVFG_PIXFMT_Y210 : GVFG_PIXFMT_YUY2;
                const bool matches = !expectedDepth
                    || (frame.bit_depth == expectedDepth && frame.pixel_format == expectedFormat);
                if (valid && !matches)
                    result.videoError = QString("預期 %1-bit %2，收到 %3-bit %4")
                        .arg(expectedDepth).arg(QString::fromUtf8(gvfg_pixel_format_name(expectedFormat)))
                        .arg(frame.bit_depth).arg(QString::fromUtf8(gvfg_pixel_format_name(frame.pixel_format)));
                if (valid) {
                    ++result.frames;
                    if (images && matches) images->sample(frame, timer.elapsed());
                }
                const auto released = gvfg_release_channel_frame(channel.handle, channel.index, &frame);
                if (released != GVFG_OK) result.videoError = channel.error("釋放影格", released);
                else if (!valid) result.videoError = "影格資料無效";
                received = true;
            } else if (status != GVFG_ETIMEOUT) {
                result.videoError = channel.error("讀取影格", status);
            }
        }
        if (audio && result.audioError.isEmpty()) {
            gvfg_audio_frame_t frame{};
            const auto status = gvfg_read_channel_audio_frame(channel.handle, channel.index, &frame, 0);
            if (status == GVFG_OK) {
                const bool valid = frame.data && frame.channels && frame.sample_rate
                    && frame.bits_per_sample == 16 && frame.data_size >= 2ULL * frame.channels
                    && frame.data_size % (2ULL * frame.channels) == 0;
                if (valid) {
                    if (result.audioBlocks && (result.pcm.sample_rate != frame.sample_rate
                            || result.pcm.channels != frame.channels))
                        result.audioError = "取樣期間 PCM 格式變更";
                    result.pcm = {frame.sample_rate, frame.channels, frame.bits_per_sample};
                    ++result.audioBlocks;
                }
                const auto released = gvfg_release_channel_audio_frame(channel.handle, channel.index, &frame);
                if (released != GVFG_OK) result.audioError = channel.error("釋放 PCM", released);
                else if (!valid) result.audioError = "PCM 資料無效";
                received = true;
            } else if (status != GVFG_ETIMEOUT) {
                result.audioError = channel.error("讀取 PCM", status);
            }
        }
        if (!(monitorContinuity && pollEvents)
            && (!video || !result.videoError.isEmpty()) && (!audio || !result.audioError.isEmpty())) break;
        if (!received) QThread::msleep(2);
    }

    // Count all frames over the full measurement window, including delivery gaps.
    result.fps = 1000.0 * result.frames / duration;
    return result;
}

QJsonObject resultRow(const QJsonObject &card, int channel,
               const QString &label, const QString &status, const QString &detail)
{
    return {{"card", card.value("name")}, {"device", card.value("id")},
        {"channel", channel}, {"check", label}, {"result", status}, {"detail", detail}};
}

void checkChannel(QJsonArray &results, const QJsonObject &card, int index, bool present,
                  const QJsonArray &selected, const QJsonObject &settings, const std::atomic_bool &stop,
                  const std::function<void(const QString &)> &progress,
                  const std::function<void(const QString &)> &log,
                  const std::function<void(const QJsonObject &)> &rowUpdate, int depth = 0)
{
    const auto chosen = [&](const QString &id) { return selected.contains(id); };
    const auto standard = [&](const CardCheck &check) { return !check.captureDepth && chosen(check.id); };
    const auto publish = [&](const QString &label, const QString &status, const QString &detail) {
        const auto row = resultRow(card, index, depth ? QString("%1-bit · ").arg(depth) + label : label, status, detail);
        if (status != "測試中") results.append(row);
        if (rowUpdate) rowUpdate(row);
    };
    const auto record = [&](const QString &label, CheckResult value, const QImage &evidence = {}) {
        if (!value.passed && !evidence.isNull()) {
            const QDir directory(settings.value("evidenceDirectory").toString());
            const auto check = depth ? QString("%1-bit · ").arg(depth) + label : label;
            QSaveFile file(directory.filePath(evidenceName(card.value("id").toString(), index, check)));
            if (!QDir().mkpath(directory.path()) || !file.open(QIODevice::WriteOnly)
                || !evidence.save(&file, "PNG") || !file.commit())
                value.detail += " · 無法保存失敗畫面";
        }
        if (!value.passed) log("測項未通過 · " + label + " · " + value.detail);
        publish(label, value.passed ? "PASS" : "FAIL", value.detail);
    };
    const auto skip = [&](const QString &label, const QString &reason) {
        publish(label, "未執行", reason);
    };
    if (std::none_of(cardChecks().begin(), cardChecks().end(), standard)) return;
    if (!present) {
        for (const auto &check : cardChecks())
            if (standard(check)) skip(check.label, "找不到選定的擷取卡");
        return;
    }
    progress(chosen("open") ? "檢查中：通道開啟" : "準備：開啟通道");
    if (chosen("open")) publish("通道開啟", "測試中", {});

    Channel channel;
    channel.index = index;
    channel.log = log;
    log("資訊 · 開啟通道");
    auto status = gvfg_create(&channel.handle);
    if (status == GVFG_OK) status = openSelectedChannel(channel.handle, card.value("id").toString(), index);
    if (status == GVFG_OK && depth) {
        status = gvfg_set_channel_video_format(channel.handle, index,
            depth == 10 ? GVFG_PIXFMT_Y210 : GVFG_PIXFMT_YUY2);
        if (status != GVFG_OK) {
            record("影像擷取與 FPS", {false, channel.error("設定擷取格式", status)});
            for (const auto &check : cardChecks())
                if (standard(check) && check.id != "video") skip(check.label, "設定擷取格式失敗");
            return;
        }
    }
    if (status != GVFG_OK) {
        const auto error = channel.error("開啟通道", status);
        for (const auto &check : cardChecks()) {
            if (!standard(check)) continue;
            if (check.id == "open" || (depth && check.id == "video"))
                record(check.label, {false, error});
            else skip(check.label, error);
        }
        return;
    }

    const bool referenceCheck = chosen("color") || chosen("reference");
    if (chosen("open")) record("通道開啟", {true, "SDK 已開啟通道"});
    const bool imageCheck = chosen("black") || chosen("freeze") || referenceCheck;
    const bool video = chosen("video") || imageCheck, audio = chosen("audio");
    ImageSamples images;
    images.monitorFreeze = chosen("freeze");
    images.saveEvidence = !settings.value("evidenceDirectory").toString().isEmpty();
    images.monitorBlack = chosen("black");
    images.monitorColor = chosen("color");
    images.monitorReference = chosen("reference");
    images.freezeLimit = settings.value("freezeMs").toInt(1000);
    images.colorLimit = settings.value("colorLimit").toDouble(15);
    images.referenceLimit = settings.value("referenceLimit").toDouble(15);
    if (referenceCheck) {
        const QImage reference(settings.value("referenceImage").toString());
        images.reference = reference;
    }
    const bool continuity = chosen("continuity");
    const bool needSignal = chosen("signal") || chosen("format") || video || audio || continuity;
    gvfg_signal_status_t signal{};
    CheckResult locked{false, {}};
    if (needSignal) {
        progress(chosen("signal") ? "檢查中：訊號鎖定" : "準備：等待訊號鎖定");
        if (chosen("signal")) publish("訊號鎖定", "測試中", {});
        locked = readSignal(channel, signal, settings.value("signalSeconds").toInt() * 1000, stop);
        if (chosen("signal") && !stop) record("訊號鎖定", locked);
    }
    if (locked.passed && chosen("format") && !stop) {
        progress("檢查中：解析度與輸入格式");
        record("解析度與輸入格式", checkFormat(signal, settings));
    }
    Capture capture;
    QString startError;
    if (locked.passed && !stop && (video || audio || continuity)) {
        QStringList samplingChecks;
        for (const auto &check : cardChecks()) {
            if (standard(check) && check.id != "open" && check.id != "signal" && check.id != "format") {
                samplingChecks.append(check.label);
                publish(check.label, "測試中", "共用取樣");
            }
        }
        progress("檢查中（共用取樣）：" + samplingChecks.join("、"));
        if (audio) {
            status = gvfg_set_channel_audio_enabled(channel.handle, index, 1);
            if (status != GVFG_OK) capture.audioError = channel.error("啟用音訊", status);
        }
        if (video || continuity || capture.audioError.isEmpty()) {
            log("資訊 · 呼叫 gvfg_start_channel");
            status = gvfg_start_channel(channel.handle, index);
            if (status != GVFG_OK) startError = channel.error("開始擷取", status);
            else {
                log("資訊 · 已開始擷取");
                const auto audioSetupError = capture.audioError;
                capture = measure(channel, video, audio && audioSetupError.isEmpty(),
                    settings.value("sampleSeconds").toInt() * 1000, stop, depth, continuity,
                    imageCheck ? &images : nullptr);
                if (!audioSetupError.isEmpty()) capture.audioError = audioSetupError;
                status = gvfg_stop_channel(channel.handle, index);
                if (status != GVFG_OK) channel.error("停止擷取", status);
            }
        }
    }

    for (const auto &check : cardChecks()) {
        if (stop || !standard(check)) continue;
        if (check.id == "open" || check.id == "signal" || (locked.passed && check.id == "format")) continue;
        if (!locked.passed && ((depth && check.id == "video") || check.id == "continuity"
                 || check.id == "black" || check.id == "freeze" || check.id == "color" || check.id == "reference"))
            record(check.label, locked);
        else if (!locked.passed) skip(check.label, locked.detail);
        else if (!startError.isEmpty()) record(check.label, {false, "開始擷取失敗：" + startError});
        else if (check.id == "video") {
            const double minimum = settings.value("minFps").toDouble();
            record(check.label, {capture.videoError.isEmpty() && capture.frames > 0 && capture.fps >= minimum,
                capture.videoError.isEmpty() ? QString("%1 影格，%2 FPS（下限 %3）")
                    .arg(capture.frames).arg(capture.fps, 0, 'f', 2).arg(minimum) : capture.videoError});
        } else if (check.id == "continuity") record(check.label, checkContinuity(capture));
        else if (check.id == "black" || check.id == "freeze" || check.id == "color" || check.id == "reference") {
            if (!capture.videoError.isEmpty()) record(check.label, {false, capture.videoError});
            else if (!images.error.isEmpty()) record(check.label, {false, images.error});
            else if (!images.count) record(check.label, {false, "取樣期間未收到可分析影格"});
            else if (check.id == "black") record(check.label, {images.black == 0,
                QString("%1／%2 張為黑畫面").arg(images.black).arg(images.count)}, images.blackEvidence);
            else if (check.id == "freeze") {
                const int limit = settings.value("freezeMs").toInt(1000);
                auto detail = QString("最長畫面未變 %1 ms，門檻 %2 ms（%3 張影格）")
                    .arg(images.freezeMs).arg(limit).arg(images.count);
                if (!images.freezeEvidence.isNull())
                    detail += QString(" · 畫面時間 %1／%2 ms").arg(images.freezeFrom).arg(images.freezeTo);
                record(check.label, {images.count > 1 && images.freezeMs < limit, detail}, images.freezeEvidence);
            } else if (images.reference.isNull()) record(check.label, {false, "無法開啟參考圖片"});
            else {
                const bool color = check.id == "color";
                const double limit = settings.value(color ? "colorLimit" : "referenceLimit").toDouble(15);
                const double value = color ? images.maxColor : images.maxDifference;
                record(check.label, {value <= limit,
                    QString("全畫面最差 16×16 區塊平均差異 %1（上限 %2）").arg(value, 0, 'f', 2).arg(limit)},
                    color ? images.colorEvidence : images.referenceEvidence);
            }
        }
        else if (!capture.audioError.isEmpty()) record(check.label, {false, capture.audioError});
        else if (!capture.audioBlocks) record(check.label, {false, "取樣期間未收到 PCM"});
        else if (check.id == "audio") {
            const int rate = settings.value("sampleRate").toInt(), channels = settings.value("audioChannels").toInt();
            record(check.label, {(!rate || capture.pcm.sample_rate == unsigned(rate))
                    && (!channels || capture.pcm.channels == unsigned(channels)),
                QString("%1 Hz / %2 聲道 / %3-bit，%4 個 PCM 區塊")
                    .arg(capture.pcm.sample_rate).arg(capture.pcm.channels)
                    .arg(capture.pcm.bits_per_sample).arg(capture.audioBlocks)});
        }
    }
}

}

const QList<CardCheck> &cardChecks()
{
    static const QList<CardCheck> checks{{"open", "通道開啟"}, {"signal", "訊號鎖定"},
        {"format", "解析度與輸入格式"}, {"video", "影像擷取與 FPS"},
        {"video8", "8-bit 擷取（YUY2）", 8}, {"video10", "10-bit 擷取（Y210）", 10},
        {"audio", "音訊擷取與 PCM 格式"},
        {"continuity", "取樣期間不得斷訊"}, {"black", "黑畫面檢查"}, {"freeze", "畫面凍結檢查"},
        {"color", "色彩比對（參考圖片）"}, {"reference", "畫面比對（參考圖片）"}};
    return checks;
}

bool imageSamplesSelfTest()
{
    for (const bool ten : {false, true}) {
        QByteArray data(ten ? 8 : 4, '\0');
        gvfg_frame_t frame{};
        frame.data = data.data();
        frame.data_size = data.size();
        frame.width = 2;
        frame.height = 1;
        frame.row_stride_bytes = int(data.size());
        frame.pixel_format = ten ? GVFG_PIXFMT_Y210 : GVFG_PIXFMT_YUY2;
        const auto fill = [&](char y, uchar cb) {
            const uchar values[]{uchar(y), cb, uchar(y), 128};
            for (int i = 0; i < 4; ++i) data[ten ? 2 * i + 1 : i] = char(values[i]);
        };
        ImageSamples images;
        images.monitorFreeze = true;
        images.saveEvidence = true;
        images.monitorBlack = images.monitorColor = images.monitorReference = true;
        images.reference = QImage(48, 48, QImage::Format_RGB32);
        images.reference.fill(Qt::black);
        fill(16, 128);
        images.sample(frame, 0);
        images.sample(frame, 1000);
        if (!images.error.isEmpty() || images.black != 2 || images.freezeMs != 1000
            || images.blackEvidence.isNull() || images.freezeEvidence.isNull()
            || images.freezeFrom != 0 || images.freezeTo != 1000
            || images.maxColor != 0 || images.maxDifference != 0) return false;
        fill(100, 200);
        images.sample(frame, 2000);
        if (images.black != 2 || images.freezeMs != 2000
            || images.colorEvidence.isNull() || images.referenceEvidence.isNull()
            || images.maxColor <= 15 || images.maxDifference <= 15) return false;
    }
    return true;
}

QJsonArray connectedCards()
{
    QJsonArray cards;
    for (const auto &device : captureDevices())
        cards.append(QJsonObject{{"id", QString::fromStdWString(device.interface_path)},
            {"name", QString::fromStdWString(device.friendly_name)}});
    return cards;
}

QJsonArray checkCards(const QJsonArray &cards, const QJsonObject &settings, const std::atomic_bool &stop,
                      const std::function<void(const QString &)> &progress,
                      const std::function<void(const QString &)> &diagnostic,
                      const std::function<void(int, int)> &workProgress,
                      const std::function<void(const QJsonObject &)> &rowUpdate)
{
    int total = int(cards.size()), completed = 0;
    for (const auto &value : cards) {
        const auto card = value.toObject();
        for (const auto &key : {"ch0", "ch1"})
            total += !card.value(key).toArray().isEmpty();
    }
    if (workProgress) workProgress(completed, total);
    const auto advance = [&] {
        if (workProgress && !stop) workProgress(++completed, total);
    };
    if (progress) progress("掃描擷取卡");
    const auto online = connectedCards();
    QJsonArray results;
    int position = 0;
    for (const auto &value : cards) {
        if (stop) break;
        const auto card = value.toObject();
        const QString prefix = QString("卡片 %1／%2 · %3").arg(++position).arg(cards.size())
            .arg(card.value("name").toString());
        if (progress) progress(prefix + " · 檢查中：擷取卡偵測");
        const bool present = std::any_of(online.begin(), online.end(), [&](const auto &device) {
            return card.value("id").toString().compare(device.toObject().value("id").toString(), Qt::CaseInsensitive) == 0;
        });
        const auto detection = resultRow(card, -1, "擷取卡偵測", present ? "PASS" : "FAIL",
            present ? "已找到 Windows 裝置介面" : "找不到本次選定的裝置介面");
        results.append(detection);
        if (rowUpdate) rowUpdate(detection);
        if (!present && diagnostic) diagnostic(prefix + " · 警告 · 找不到裝置介面");
        advance();
        for (int index = 0; index < 2 && !stop; ++index) {
            const auto selected = card.value(QString("ch%1").arg(index)).toArray();
            if (selected.isEmpty()) continue;
            const auto channelProgress = [&](const QString &step) {
                if (progress) progress(prefix + QString(" · CH%1 · ").arg(index) + step);
            };
            const auto channelLog = [&](int depth, const QString &message) {
                if (diagnostic) diagnostic(prefix + QString(" · CH%1 · ").arg(index)
                    + (depth ? QString("%1-bit · ").arg(depth) : QString()) + message);
            };
            const auto commonLog = [&](const QString &message) { channelLog(0, message); };
            if (!selected.contains("video8") && !selected.contains("video10")) {
                checkChannel(results, card, index, present, selected, settings, stop, channelProgress, commonLog, rowUpdate);
                advance();
                continue;
            }
            QJsonArray common, perFormat{"video"};
            for (const auto &check : cardChecks()) {
                if (check.captureDepth || check.id == "video" || !selected.contains(check.id)) continue;
                if (check.id != "open" && check.id != "signal" && check.id != "format") perFormat.append(check.id);
                else common.append(check.id);
            }
            checkChannel(results, card, index, present, common, settings, stop, channelProgress, commonLog, rowUpdate);
            for (const auto &check : cardChecks()) {
                if (stop || !check.captureDepth || !selected.contains(check.id)) continue;
                checkChannel(results, card, index, present, perFormat, settings, stop, [&](const QString &step) {
                    channelProgress(check.label + " · " + step);
                }, [&](const QString &message) { channelLog(check.captureDepth, message); }, rowUpdate, check.captureDepth);
            }
            advance();
        }
    }
    return results;
}

bool roundPassed(const QJsonArray &results)
{
    return !results.isEmpty() && std::all_of(results.begin(), results.end(), [](const auto &item) {
        return item.toObject().value("result").toString() == "PASS";
    });
}
