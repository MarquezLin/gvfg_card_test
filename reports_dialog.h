#pragma once

#include <QString>
#include <QJsonObject>

QString reportStatistics(const QString &directory, const QJsonObject &active);

class QWidget;

void showReports(QWidget *parent, const QString &directory, const QString &currentReport,
                 const QString &protectedReport);
bool reportSelfTest(const QString &directory);
