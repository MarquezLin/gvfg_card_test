#include "session.h"
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
const auto runKey = R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run)";
const auto startupName = "GVFG_CardTest";

QString startupCommand()
{
    return '"' + QDir::toNativeSeparators(QCoreApplication::applicationFilePath()) + '"';
}

bool updateStartup(const QString &command, QString &error)
{
    QSettings run(runKey, QSettings::NativeFormat);
    if (command.size() > 260) {
        error = "程式路徑太長，Windows 啟動命令超過 260 字元";
        return false;
    }
    if (command.isEmpty()) run.remove(startupName);
    else run.setValue(startupName, command);
    run.sync();
    const bool ok = run.status() == QSettings::NoError
        && (command.isEmpty() ? !run.contains(startupName) : run.value(startupName).toString() == command);
    if (!ok) error = "無法更新目前使用者的 Windows 自動啟動設定";
    return ok;
}
}

bool setLoginStartup(bool enabled, QString &error)
{
    return updateStartup(enabled ? startupCommand() : QString(), error);
}

bool setResumeStartup(bool enabled, QString &error)
{
    QSettings run(runKey, QSettings::NativeFormat);
    const auto existing = run.value(startupName).toString();
    const auto command = startupCommand();
    if (existing == command) return true;
    if (!enabled && existing != command + " --resume") return true;
    if (enabled && !existing.isEmpty() && existing != command + " --resume") {
        error = "自動啟動路徑已變更，請按「安裝自動啟動」更新後重試。";
        return false;
    }
    return updateStartup(enabled ? command + " --resume" : QString(), error);
}

QString loginStartupStatus()
{
    QSettings run(runKey, QSettings::NativeFormat);
    const auto existing = run.value(startupName).toString();
    if (run.status() != QSettings::NoError) return "無法讀取 Windows 自動啟動設定";
    if (existing.isEmpty()) return "尚未安裝";
    if (existing == startupCommand()) return "已安裝，登入後開啟";
    if (existing == startupCommand() + " --resume") return "已登記本次冷開機續跑";
    return "程式路徑已變更，請重新安裝";
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
