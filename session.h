#pragma once

#include "relay_controller.h"
#include <QJsonObject>
#include <atomic>
#include <functional>

bool saveJson(const QString &path, const QJsonObject &object);
QJsonObject loadJson(const QString &path);
bool setResumeStartup(bool enabled, QString &error);
bool setLoginStartup(bool enabled, QString &error);
QString loginStartupStatus();
RelaySettings relayFromConfig(const QJsonObject &config);
QString scheduleShutdown(const RelaySettings &relay, const QString &requestId,
                         const std::atomic_bool &stop, bool &mayBePending,
                         const std::function<void(const QString &, bool)> &reconnect = {});
bool writeRoundReport(const QString &directory, int round, const QJsonArray &results,
                      bool stopped);
QString roundLogName(int round);
QString evidenceName(const QString &device, int channel, const QString &check);
