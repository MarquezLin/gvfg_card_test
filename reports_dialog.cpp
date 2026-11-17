#include "reports_dialog.h"
#include "session.h"

#include <QComboBox>
#include <QCheckBox>
#include <QIcon>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTextStream>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace {
const QStringList headings{"輪次", "時間", "擷取卡", "裝置介面", "通道", "測項", "結果", "詳細資訊"};
const QRegularExpression roundPattern(R"(^round_([0-9]+)\.csv$)");

struct Report
{
    QString path, title, ending;
    QDateTime started;
};

QString reportPath(const QDir &root, const QString &name)
{
    const QUuid id(name);
    if (id.isNull() || id.toString(QUuid::WithoutBraces) != name) return {};
    const QFileInfo entry(root.filePath(name));
    if (!entry.isDir() || entry.isSymLink() || entry.isJunction()
        || entry.canonicalPath().compare(root.canonicalPath(), Qt::CaseInsensitive) != 0)
        return {};
    return entry.canonicalFilePath();
}

QStringList roundFiles(const QString &path)
{
    QStringList rounds;
    for (const auto &entry : QDir(path).entryInfoList({"round_*.csv"}, QDir::Files | QDir::NoSymLinks)) {
        if (roundPattern.match(entry.fileName()).hasMatch()) rounds.append(entry.fileName());
    }
    std::sort(rounds.begin(), rounds.end(), [](const QString &a, const QString &b) {
        return roundPattern.match(a).captured(1).toInt() < roundPattern.match(b).captured(1).toInt();
    });
    return rounds;
}

QList<QStringList> readRows(const QString &path, bool &ok)
{
    QFile file(path);
    ok = file.open(QIODevice::ReadOnly);
    if (!ok) return {};
    QString text = QString::fromUtf8(file.readAll());
    if (text.startsWith(QChar::ByteOrderMark)) text.remove(0, 1);
    QList<QStringList> rows;
    QStringList fields;
    QString field;
    bool quoted = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar ch = text[i];
        if (ch == '"') {
            if (quoted && i + 1 < text.size() && text[i + 1] == '"') {
                field.append(ch);
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (!quoted && (ch == ',' || ch == '\n')) {
            if (ch == '\n' && field.endsWith('\r')) field.chop(1);
            fields.append(field);
            field.clear();
            if (ch == '\n') {
                rows.append(fields);
                fields.clear();
            }
        } else {
            field.append(ch);
        }
    }
    if (!field.isEmpty() || !fields.isEmpty()) {
        fields.append(field);
        rows.append(fields);
    }
    ok = file.error() == QFile::NoError && !quoted && !rows.isEmpty() && rows.first().size() == headings.size()
        && std::all_of(rows.cbegin(), rows.cend(), [](const auto &row) { return row.size() == headings.size(); });
    if (!ok) return {};
    rows.removeFirst();
    return rows;
}

constexpr int resultColumn = 6;

QString evidencePath(const QDir &directory, const QString &csv, const QStringList &row)
{
    if (row.at(resultColumn) != "FAIL") return {};
    const auto channel = row.at(4);
    if (channel != "CH0" && channel != "CH1") return {};
    const auto path = directory.filePath(QFileInfo(csv).completeBaseName() + "_images/"
        + evidenceName(row.at(3), channel.back().digitValue(), row.at(5)));
    return QFileInfo(path).isFile() ? path : QString();
}

QString readRoundLog(const QDir &directory, const QString &csv)
{
    QFile file(directory.filePath(roundLogName(roundPattern.match(csv).captured(1).toInt())));
    return file.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(file.readAll()) : QString();
}

QIcon resultLamp(const QString &status)
{
    const QColor color(status == "PASS" ? "#15803d" : status == "FAIL" ? "#b91c1c"
        : status == "已停止" ? "#b45309" : "#64748b");
    QPixmap dot(14, 14);
    dot.fill(Qt::transparent);
    QPainter painter(&dot);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QRect(2, 2, 10, 10));
    painter.end();
    return QIcon(dot);
}

QString roundStatus(const QList<QStringList> &rows, bool readable)
{
    if (!readable || rows.isEmpty()) return "無結果";
    bool failed = false;
    for (const auto &row : rows) {
        if (row.at(resultColumn) == "已停止") return "已停止";
        failed |= row.at(resultColumn) != "PASS";
    }
    return failed ? "FAIL" : "PASS";
}

bool exportHtml(const Report &report, const QString &destination)
{
    QSaveFile file(destination);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QTextStream html(&file);
    html << "<!doctype html><html lang='zh-Hant'><meta charset='utf-8'>"
            "<title>GVFG 測試報告</title><style>"
            "body{font:15px system-ui;margin:2rem;color:#0f172a}h1,h2{margin:.8em 0}"
            "table{border-collapse:collapse;width:100%}td,th{border:1px solid #cbd5e1;"
            "text-align:left;padding:.4em;white-space:pre-wrap;overflow-wrap:anywhere}"
            "th{background:#f1f5f9}</style><h1>GVFG 測試報告</h1><p>"
         << report.title.toHtmlEscaped() << "</p><p>" << report.ending.toHtmlEscaped() << "</p>";
    const auto rounds = roundFiles(report.path);
    for (const auto &round : rounds) {
        bool ok = false;
        const auto rows = readRows(QDir(report.path).filePath(round), ok);
        if (!ok) return false;
        html << "<h2>第 " << roundPattern.match(round).captured(1).toInt() << " 輪</h2><table><thead><tr>";
        for (const auto &heading : headings) html << "<th>" << heading << "</th>";
        html << "</tr></thead><tbody>";
        for (const auto &row : rows) {
            html << "<tr>";
            for (int column = 0; column < row.size(); ++column) {
                html << "<td>" << row[column].toHtmlEscaped();
                if (column == 7) {
                    const auto path = evidencePath(QDir(report.path), round, row);
                    QFile image(path);
                    if (!path.isEmpty() && image.open(QIODevice::ReadOnly))
                        html << "<details><summary>失敗畫面</summary><img alt='失敗畫面' style='max-width:100%' src='data:image/png;base64,"
                             << QString::fromLatin1(image.readAll().toBase64()) << "'></details>";
                }
                html << "</td>";
            }
            html << "</tr>";
        }
        html << "</tbody></table>";
        const auto log = readRoundLog(QDir(report.path), round);
        if (!log.isEmpty()) html << "<h3>SDK 紀錄</h3><pre style='white-space:pre-wrap'>"
                                 << log.toHtmlEscaped() << "</pre>";
    }
    html << "</html>";
    html.flush();
    return html.status() == QTextStream::Ok && file.commit();
}

bool exportCsv(const QString &source, const QString &destination)
{
    QFile input(source);
    QSaveFile output(destination);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) return false;
    const auto data = input.readAll();
    return input.error() == QFile::NoError && output.write(data) == data.size() && output.commit();
}
}

QString reportStatistics(const QString &directory, const QJsonObject &active)
{
    quint64 formal = 0, trial = 0, passed = 0, failed = 0, stopped = 0, incomplete = 0;
    const QDir root(directory);
    for (const auto &category : {QString("cold"), QString("trial")}) {
        const QDir group(root.filePath(category));
        for (const auto &id : group.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
            const auto path = reportPath(group, id);
            if (path.isEmpty()) continue;
            const QDir session(path);
            const auto state = active.value("id").toString() == id ? active
                : loadJson(session.filePath("session.json"));
            const auto files = roundFiles(path);
            auto &count = category == "cold" ? formal : trial;
            count += files.size();
            for (const auto &file : files) {
                bool ok = false;
                const auto rows = readRows(session.filePath(file), ok);
                const auto status = roundStatus(rows, ok);
                if (status == "PASS") ++passed;
                else if (status == "FAIL") ++failed;
                else if (status == "已停止") ++stopped;
                else ++incomplete;
            }
            const auto phase = state.value("phase").toString();
            const int started = state.value("lastStartedRound").toInt(
                phase == "running" ? state.value("round").toInt()
                : files.isEmpty() && phase == "finished" ? 1 : 0);
            const int lastSaved = files.isEmpty() ? 0 : roundPattern.match(files.last()).captured(1).toInt();
            if (started > lastSaved) {
                ++count;
                ++incomplete;
            }
        }
    }
    return QString("累計 %1 次（正式 %2／試跑 %3）｜PASS %4｜FAIL %5｜停止 %6｜未完成 %7")
        .arg(formal + trial).arg(formal).arg(trial).arg(passed).arg(failed).arg(stopped).arg(incomplete);
}

void showReports(QWidget *parent, const QString &directory, const QString &currentReport,
                 const QString &protectedReport)
{
    const QDir root(directory);
    QDialog dialog(parent);
    dialog.setObjectName("reportDialog");
    dialog.setWindowTitle("GVFG 測試報告");
    dialog.resize(1140, 680);
    QVBoxLayout layout(&dialog);
    QSplitter split(Qt::Horizontal);
    QListWidget sessions;
    QWidget detail;
    QVBoxLayout detailLayout(&detail);
    detailLayout.setContentsMargins(4, 0, 0, 0);
    QLabel summary;
    summary.setWordWrap(true);
    QComboBox rounds;
    QCheckBox failedOnly("只看未通過");
    failedOnly.setToolTip("隱藏已通過的輪次與測項");
    QTableWidget table(0, int(headings.size()));
    table.setObjectName("reportTable");
    table.setHorizontalHeaderLabels(headings);
    table.setAlternatingRowColors(true);
    table.setEditTriggers(QAbstractItemView::NoEditTriggers);
    table.setSelectionBehavior(QAbstractItemView::SelectRows);
    table.verticalHeader()->hide();
    table.horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table.horizontalHeader()->setSectionResizeMode(7, QHeaderView::Stretch);
    table.setColumnHidden(3, true);
    table.setColumnWidth(0, 55);
    table.setColumnWidth(1, 158);
    table.setColumnWidth(2, 120);
    table.setColumnWidth(4, 52);
    table.setColumnWidth(5, 135);
    table.setColumnWidth(6, 100);
    detailLayout.addWidget(&summary);
    auto *filters = new QHBoxLayout;
    filters->addWidget(&rounds, 1);
    filters->addWidget(&failedOnly);
    detailLayout.addLayout(filters);
    detailLayout.addWidget(&table, 1);
    QPlainTextEdit diagnostics;
    diagnostics.setObjectName("reportDiagnostics");
    diagnostics.setReadOnly(true);
    diagnostics.setPlaceholderText("本輪沒有 SDK 紀錄");
    diagnostics.setMaximumHeight(155);
    detailLayout.addWidget(new QLabel("本輪 SDK 紀錄"));
    detailLayout.addWidget(&diagnostics);
    split.addWidget(&sessions);
    split.addWidget(&detail);
    split.setSizes({260, 880});
    layout.addWidget(&split, 1);

    QHBoxLayout actions;
    QPushButton folder("開啟報告資料夾"), html("匯出整場 HTML…"), csv("匯出本輪 CSV…"),
        remove("刪除報告…"), close("關閉");
    remove.setStyleSheet("QPushButton:enabled {color: #b52323;}");
    actions.addWidget(&folder);
    actions.addStretch();
    for (auto *button : {&html, &csv, &remove, &close}) actions.addWidget(button);
    layout.addLayout(&actions);
    QList<Report> reports;
    QList<QPair<QString, QString>> roundStatuses;

    const auto reload = [&] {
        QSignalBlocker blocker(&sessions);
        sessions.clear();
        reports.clear();
        for (const QString &category : {QString("trial"), QString("cold")}) {
          const QDir group(root.filePath(category));
          for (const auto &name : group.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
            const auto path = reportPath(group, name);
            if (path.isEmpty()) continue;
            const auto state = loadJson(path + "/session.json");
            const auto settings = loadJson(path + "/settings.json");
            if (settings.isEmpty() && state.isEmpty() && roundFiles(path).isEmpty()) continue;
            auto started = QDateTime::fromString(state.value("startedAt").toString(), Qt::ISODateWithMs);
            if (!started.isValid()) started = QFileInfo(path).birthTime();
            const auto mode = category == "cold" ? "冷開機" : "單輪試跑";
            const auto ending = path.compare(protectedReport, Qt::CaseInsensitive) == 0
                ? "測試尚在進行或等待續跑"
                : state.value("ending").toString("結束狀態未保存");
            reports.append({path, started.toLocalTime().toString("yyyy-MM-dd HH:mm:ss") + " · " + mode,
                ending, started});
          }
        }
        std::sort(reports.begin(), reports.end(), [](const Report &a, const Report &b) { return a.started > b.started; });
        for (const auto &report : reports) {
            auto *item = new QListWidgetItem(report.title, &sessions);
            item->setToolTip(report.path);
        }
    };

    const auto refreshRound = [&] {
        table.setRowCount(0);
        diagnostics.clear();
        const int index = sessions.currentRow();
        const auto file = rounds.currentData().toString();
        csv.setEnabled(index >= 0 && !file.isEmpty());
        if (index < 0 || file.isEmpty()) return;
        diagnostics.setPlainText(readRoundLog(QDir(reports[index].path), file));
        bool ok = false;
        auto rows = readRows(QDir(reports[index].path).filePath(file), ok);
        if (failedOnly.isChecked()) {
            rows.erase(std::remove_if(rows.begin(), rows.end(), [](const auto &row) {
                return row.at(resultColumn) == "PASS";
            }), rows.end());
        }
        summary.setText(reports[index].ending + (ok ? QString() : " · 無法讀取本輪 CSV"));
        table.setRowCount(int(rows.size()));
        for (int row = 0; row < rows.size(); ++row) {
            const auto image = evidencePath(QDir(reports[index].path), file, rows[row]);
            for (int col = 0; col < headings.size(); ++col) {
                auto *cell = new QTableWidgetItem(rows[row][col]);
                if (col == resultColumn) cell->setIcon(resultLamp(rows[row][col]));
                cell->setToolTip(col == 2 ? rows[row][3] : rows[row][col]);
                if (col == resultColumn && !image.isEmpty()) {
                    cell->setData(Qt::UserRole, image);
                    cell->setToolTip("雙擊查看失敗畫面");
                }
                if (col == 7 && !image.isEmpty()) cell->setText("有失敗畫面 · " + cell->text());
                table.setItem(row, col, cell);
            }
        }
    };

    const auto refreshRounds = [&](const QString &preferred) {
        QSignalBlocker blocker(&rounds);
        rounds.clear();
        const int index = sessions.currentRow();
        if (index < 0) {
            summary.setText("目前沒有測試報告。");
        } else {
            summary.setText(reports[index].ending);
            for (const auto &[file, status] : roundStatuses) {
                if (failedOnly.isChecked() && status == "PASS") continue;
                rounds.addItem(resultLamp(status),
                    QString("第 %1 輪 · %2").arg(roundPattern.match(file).captured(1).toInt()).arg(status), file);
            }
            if (!rounds.count()) {
                rounds.addItem(roundStatuses.isEmpty() ? "尚未保存任何輪次" : "所有已保存輪次皆通過");
            } else {
                const int preferredIndex = rounds.findData(preferred);
                rounds.setCurrentIndex(preferredIndex >= 0 ? preferredIndex : rounds.count() - 1);
            }
        }
        refreshRound();
    };

    const auto refreshSession = [&](int index) {
        roundStatuses.clear();
        folder.setEnabled(index >= 0);
        remove.setEnabled(index >= 0 && reports[index].path.compare(protectedReport, Qt::CaseInsensitive) != 0);
        if (index >= 0) {
            const QDir dir(reports[index].path);
            for (const auto &file : roundFiles(dir.path())) {
                bool ok = false;
                const auto rows = readRows(dir.filePath(file), ok);
                roundStatuses.append({file, roundStatus(rows, ok)});
            }
        }
        html.setEnabled(!roundStatuses.isEmpty());
        failedOnly.setEnabled(!roundStatuses.isEmpty());
        refreshRounds({});
    };

    QObject::connect(&sessions, &QListWidget::currentRowChanged, &dialog, refreshSession);
    QObject::connect(&rounds, &QComboBox::currentIndexChanged, &dialog, refreshRound);
    QObject::connect(&table, &QTableWidget::cellDoubleClicked, &dialog, [&](int row, int) {
        const QPixmap image(table.item(row, resultColumn)->data(Qt::UserRole).toString());
        if (image.isNull()) return;
        QDialog viewer(&dialog);
        viewer.setWindowTitle(table.item(row, 5)->text());
        QVBoxLayout viewLayout(&viewer);
        auto *description = new QLabel(table.item(row, 7)->text());
        description->setWordWrap(true);
        viewLayout.addWidget(description);
        auto *scroll = new QScrollArea;
        auto *preview = new QLabel;
        preview->setPixmap(image);
        scroll->setWidget(preview);
        viewLayout.addWidget(scroll, 1);
        viewer.resize(1050, 650);
        viewer.exec();
    });
    QObject::connect(&failedOnly, &QCheckBox::toggled, &dialog, [&] { refreshRounds(rounds.currentData().toString()); });
    QObject::connect(&folder, &QPushButton::clicked, &dialog, [&] {
        if (sessions.currentRow() >= 0)
            QDesktopServices::openUrl(QUrl::fromLocalFile(reports[sessions.currentRow()].path));
    });
    QObject::connect(&html, &QPushButton::clicked, &dialog, [&] {
        const auto report = reports[sessions.currentRow()];
        const auto destination = QFileDialog::getSaveFileName(&dialog, "匯出整場 HTML",
            root.filePath("exports/" + QFileInfo(report.path).fileName() + ".html"), "HTML (*.html)");
        if (!destination.isEmpty() && !exportHtml(report, destination))
            QMessageBox::warning(&dialog, "匯出失敗", "無法讀取輪次資料或寫入 HTML。");
    });
    QObject::connect(&csv, &QPushButton::clicked, &dialog, [&] {
        const auto file = rounds.currentData().toString();
        const auto destination = QFileDialog::getSaveFileName(&dialog, "匯出本輪 CSV",
            root.filePath("exports/" + QFileInfo(reports[sessions.currentRow()].path).fileName() + "_" + file),
            "CSV (*.csv)");
        if (!destination.isEmpty() && !exportCsv(QDir(reports[sessions.currentRow()].path).filePath(file), destination))
            QMessageBox::warning(&dialog, "匯出失敗", "無法讀取或寫入 CSV。");
    });
    QObject::connect(&remove, &QPushButton::clicked, &dialog, [&] {
        const int index = sessions.currentRow();
        if (index < 0) return;
        const auto report = reports[index];
        if (report.path.compare(protectedReport, Qt::CaseInsensitive) == 0) return;
        if (QMessageBox::question(&dialog, "刪除測試報告",
                "將這場測試的所有輪次移到資源回收筒？\n" + report.title,
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        const QFileInfo entry(report.path);
        const auto path = reportPath(entry.dir(), entry.fileName());
        if (path.isEmpty() || !QFile::moveToTrash(path)) {
            QMessageBox::warning(&dialog, "刪除失敗", "無法將報告移到資源回收筒。");
            return;
        }
        reload();
        if (reports.isEmpty()) refreshSession(-1);
        else sessions.setCurrentRow(qMin(index, int(reports.size()) - 1));
    });
    QObject::connect(&close, &QPushButton::clicked, &dialog, &QDialog::accept);
    reload();
    int initial = 0;
    for (int index = 0; index < reports.size(); ++index) {
        if (reports[index].path.compare(currentReport, Qt::CaseInsensitive) == 0) {
            initial = index;
            break;
        }
    }
    if (reports.isEmpty()) refreshSession(-1);
    else sessions.setCurrentRow(initial);
    dialog.exec();
}

bool reportSelfTest(const QString &directory)
{
    const QString name = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QDir root(directory);
    if (!root.mkdir(name)) return false;
    const auto path = reportPath(root, name);
    if (path.isEmpty() || !reportPath(root, "../outside").isEmpty()) return false;
    const QJsonArray rows{QJsonObject{{"card", "測試卡"}, {"channel", 1}, {"check", "影像"},
        {"result", "FAIL"}, {"detail", "逗號,引號\"\r\n<測試>"}}};
    if (!writeRoundReport(path, 2, rows, false) || !writeRoundReport(path, 10, {}, true)) return false;
    const QString source = QDir(path).filePath("round_0002.csv");
    bool ok = false;
    const auto parsed = readRows(source, ok);
    if (!ok || parsed.size() != 1 || parsed.first().value(7) != "逗號,引號\"\r\n<測試>"
        || roundStatus(parsed, ok) != "FAIL") return false;
    const QDir reportDir(path);
    if (!reportDir.mkdir("round_0002_images")) return false;
    const auto evidence = reportDir.filePath("round_0002_images/" + evidenceName({}, 1, "影像"));
    QImage fixture(2, 2, QImage::Format_RGB32);
    fixture.fill(Qt::red);
    if (!fixture.save(evidence) || evidencePath(reportDir, "round_0002.csv", parsed.first()) != evidence)
        return false;
    auto pass = parsed;
    pass.first()[resultColumn] = "PASS";
    const auto stopped = readRows(QDir(path).filePath("round_0010.csv"), ok);
    if (!ok || roundStatus(stopped, ok) != "已停止" || roundStatus(pass, true) != "PASS"
        || roundStatus({}, false) != "無結果") return false;
    const auto html = root.filePath("report.html"), csv = root.filePath("report.csv");
    if (!exportHtml({path, "測試", "完成", {}}, html) || !exportCsv(source, csv)) return false;
    QFile output(html), input(source), copy(csv);
    if (!output.open(QIODevice::ReadOnly) || !input.open(QIODevice::ReadOnly) || !copy.open(QIODevice::ReadOnly))
        return false;
    const auto content = output.readAll();
    const auto first = content.indexOf(QString("第 2 輪").toUtf8());
    return input.readAll() == copy.readAll() && content.contains(QString("&lt;測試&gt;").toUtf8())
        && content.contains("data:image/png;base64,")
        && first >= 0 && content.indexOf(QString("第 10 輪").toUtf8()) > first;
}
