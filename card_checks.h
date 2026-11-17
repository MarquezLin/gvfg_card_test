#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <atomic>
#include <functional>

struct CardCheck
{
    QString id;
    QString label;
    int captureDepth = 0;
};

const QList<CardCheck> &cardChecks();
QJsonArray connectedCards();
QJsonArray checkCards(const QJsonArray &cards, const QJsonObject &settings,
                      const std::atomic_bool &stop,
                      const std::function<void(const QString &)> &progress = {},
                      const std::function<void(const QString &)> &diagnostic = {},
                      const std::function<void(int, int)> &workProgress = {},
                      const std::function<void(const QJsonObject &)> &rowUpdate = {});
bool roundPassed(const QJsonArray &results);
bool imageSamplesSelfTest();
