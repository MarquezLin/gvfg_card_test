#include "session.h"
#include "card_startup_manager.h"
#include <QCryptographicHash>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QThread>
#include <windows.h>

namespace {
bool saveBytes(const QString &path, const QByteArray &data)
{
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}

QString csv(QString value)
{
    value.replace('"', "\"\"");
    return '"' + value + '"';
}
}

bool saveJson(const QString &path, const QJsonObject &object)
{
    return saveBytes(path, QJsonDocument(object).toJson());
}

QJsonObject loadJson(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}

namespace {
CardStartupManager startupManager()
{
    return CardStartupManager(std::filesystem::path(QCoreApplication::applicationFilePath().toStdWString()));
}
}

bool setLoginStartup(bool enabled, QString &error)
{
    auto manager = startupManager();
    std::wstring problem;
    if (!(enabled ? manager.installStartup(problem) : manager.uninstallStartup(problem))) {
        error = QString::fromStdWString(problem);
        return false;
    }
    QSettings run(R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run)", QSettings::NativeFormat);
    run.remove("GVFG_CardTest");
    run.sync();
    if (run.status() != QSettings::NoError) {
        error = "無法清除舊的 Run 自動啟動設定";
        return false;
    }
    return true;
}

bool setResumeStartup(bool enabled, QString &error)
{
    // Keep the installed login shortcut after a test finishes, like boot_cycle_test.
    return !enabled || setLoginStartup(true, error);
}

QString loginStartupStatus()
{
    return QString::fromStdWString(startupManager().queryStartup().text);
}

RelaySettings relayFromConfig(const QJsonObject &config)
{
    RelaySettings settings;
    settings.agentUrl = config.value("agent").toString();
    if (settings.agentUrl.trimmed().isEmpty()) settings.agentUrl = "http://192.168.50.20:8080";
    settings.delaySeconds = config.value("shutdownDelay").toInt(30);
    settings.ticks = config.value("offTicks").toInt(200);
    return settings;
}

QString scheduleShutdown(const RelaySettings &relay, const QString &id,
                         const std::atomic_bool &stop, bool &mayBePending,
                         const std::function<void(const QString &, bool)> &reconnect)
{
    mayBePending = false;
    int channel = 0;
    QString error;
    const auto retry = [&](const QString &phase, const auto &request) {
        bool reconnecting = false;
        while (!stop) {
            bool retryable = false;
            if (request(retryable)) {
                if (reconnecting && reconnect) reconnect(phase + " · Relay Agent 已重連", false);
                return true;
            }
            if (!retryable || stop) break;
            reconnecting = true;
            if (reconnect) reconnect(phase + " · 等待 Relay Agent 重連：" + error, true);
            for (int tick = 0; tick < 30 && !stop; ++tick) QThread::msleep(100);
        }
        return false;
    };
    if (!retry("關機前連線檢查", [&](bool &retryable) {
        return checkRelayAgent(relay, channel, error, true, &retryable);
    })) return stop ? "已停止" : "Relay Agent 檢查失敗：" + error;
    if (stop) return "已停止";
    QString requestId = id;
    bool stoppedByAgent = false;
    bool submitted = false;
    const bool accepted = retry("斷電排程", [&](bool &retryable) {
        submitted = true;
        return scheduleRelayAgent(relay, requestId, error, stoppedByAgent, &retryable);
    });
    if (accepted && !stop) {
        wchar_t systemDirectory[MAX_PATH]{};
        const auto length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
        if (length && length < MAX_PATH) {
            const QString shutdown = QString::fromWCharArray(systemDirectory) + "/shutdown.exe";
            if (QProcess::execute(shutdown, {"/s", "/t", "0", "/d", "p:4:1",
                    "/c", "GVFG capture card cold boot test"}) == 0) {
                mayBePending = true;
                return {};
            }
        }
        error = "Windows 關機命令失敗";
    } else if (stop) {
        error = "已要求停止";
    }
    if (submitted && !stoppedByAgent) {
        QString cancelError;
        if (!cancelRelayAgent(relay, id, cancelError)) {
            mayBePending = true;
            error += "。無法確認 Relay Agent 已取消排程，請在控制端確認。";
        }
    }
    return error;
}

bool writeRoundReport(const QString &directory, int round, const QJsonArray &results, bool stopped)
{
    const QString stamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    QString rows = "輪次,時間,卡片,裝置介面,通道,測項,結果,詳細資訊\r\n";
    for (const auto &entry : results) {
        const auto row = entry.toObject();
        const int channel = row.value("channel").toInt();
        const QStringList fields{QString::number(round), stamp, row.value("card").toString(),
            row.value("device").toString(), channel < 0 ? QString() : QString("CH%1").arg(channel),
            row.value("check").toString(), row.value("result").toString(), row.value("detail").toString()};
        QStringList escaped;
        for (const auto &field : fields) escaped.append(csv(field));
        rows += escaped.join(',') + "\r\n";
    }
    if (stopped) rows += QString("%1,%2,,,,,已停止,本輪未完成\r\n").arg(round).arg(csv(stamp));
    const QString file = QDir(directory).filePath(QString("round_%1.csv").arg(round, 4, 10, QChar('0')));
    return saveBytes(file, QByteArray::fromHex("efbbbf") + rows.toUtf8());
}

QString roundLogName(int round)
{
    return QString("round_%1.log").arg(round, 4, 10, QChar('0'));
}

QString evidenceName(const QString &device, int channel, const QString &check)
{
    const auto key = device + '\n' + QString::number(channel) + '\n' + check;
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex()) + ".png";
}
