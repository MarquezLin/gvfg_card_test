#include "card_checks.h"
#include "gvfg_capture.h"
#include "reports_dialog.h"
#include "session.h"
#include "ui_card_test_window.h"

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QProcess>
#include <cstdio>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QImageReader>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLockFile>
#include <QMessageBox>
#include <QHostInfo>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPushButton>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QSet>
#include <QSplitter>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <functional>
#include <memory>

namespace {
constexpr int identityRole = Qt::UserRole;
constexpr int checkRole = Qt::UserRole + 1;

QString roundName(int round)
{
    return QString("第 %1 輪").arg(round);
}

QString reportCategory(const QJsonObject &settings)
{
    return settings.value("cold").toBool() ? "cold" : "trial";
}

bool prepareReports(const QString &legacyRoot, const QString &destination)
{
    const QDir reports(destination);
    for (const QString &category : {QString("trial"), QString("cold"), QString("exports")})
        if (!QDir().mkpath(reports.filePath(category))) return false;
    const QDir old(legacyRoot);
    if (!old.exists() || old.canonicalPath() == reports.canonicalPath()) return true;
    const auto active = loadJson(old.filePath("../active.json"));
    for (const auto &entry : old.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        const auto name = entry.fileName();
        const QUuid id(name);
        if (id.isNull() || id.toString(QUuid::WithoutBraces) != name || entry.isJunction()) continue;
        const auto settings = loadJson(entry.filePath() + "/settings.json");
        const auto state = loadJson(entry.filePath() + "/session.json");
        const auto fallback = active.value("id").toString() == name ? active : state;
        const auto category = reportCategory(settings.isEmpty() ? fallback.value("settings").toObject() : settings);
        if (!QDir().rename(entry.absoluteFilePath(), reports.filePath(category + '/' + name))) return false;
    }
    return true;
}

int runCaptureWorker(QCoreApplication &app, const QString &directory)
{
    const QDir work(directory);
    const auto options = loadJson(work.filePath("options.json"));
    if (options.isEmpty()) return 2;

    std::atomic_bool stop{false};
    const auto send = [](const QJsonObject &message) {
        const auto line = QJsonDocument(message).toJson(QJsonDocument::Compact);
        std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    };
    int exitCode = 2;
    auto *worker = QThread::create([&] {
        const auto results = checkCards(options.value("cards").toArray(), options, stop,
            [&](const QString &step) { send({{"step", step}}); },
            [&](const QString &message) { send({{"log", message}}); },
            [&](int done, int total) { send({{"done", done}, {"total", total}}); },
            [&](const QJsonObject &row) { send({{"row", row}}); });
        exitCode = saveJson(work.filePath("result.json"), {{"rows", results}}) ? 0 : 2;
    });
    QObject::connect(worker, &QThread::finished, &app, [&] { app.exit(exitCode); });
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (QFile::exists(work.filePath("stop"))) stop = true;
    });
    timer.start(50);
    worker->start();
    const int result = app.exec();
    worker->wait();
    delete worker;
    return result;
}
class CardTestWindow final : public QWidget
{
public:
    CardTestWindow(const QString &root, const QString &reportsRoot)
        : root(root), activeFile(root + "/active.json"), reportsRoot(reportsRoot)
    {
        ui.setupUi(this);
        const auto *sdkVersion = gvfg_get_version();
        ui.sdkVersionLabel->setText(QString("GVFG SDK %1")
            .arg(sdkVersion && *sdkVersion ? QString::fromUtf8(sdkVersion) : QString("未知")));
        ui.sdkVersionLabel->setToolTip("目前載入的 gvfg.dll 版本");
        resize(1200, 740);
        ui.workspaceSplitter->setStretchFactor(0, 3);
        ui.workspaceSplitter->setStretchFactor(1, 2);
        ui.workspaceSplitter->setSizes({700, 460});
        tree = ui.cardTree;
        tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int column = 1; column <= 2; ++column)
            tree->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
        results = ui.resultsTable;
        results->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        status = ui.statusLabel;
        stopButton = ui.stopButton;
        rounds = ui.roundsSpin;
        keepGoing = ui.keepGoingCheck;
        config = loadJson(root + "/settings.json");
        rounds->setValue(config.value("rounds").toInt(2));
        keepGoing->setChecked(config.value("keepGoing").toBool());
        rebuildCards(config.value("cards").toArray());

        connect(ui.refreshButton, &QPushButton::clicked, this, [this] { rebuildCards(selectedCards()); });
        connect(ui.copyButton, &QPushButton::clicked, this, [this] { copyChecks(); });
        connect(ui.thresholdsButton, &QPushButton::clicked, this, [this] { editThresholds(); });
        connect(ui.agentButton, &QPushButton::clicked, this, [this] { editAgent(); });
        connect(ui.installStartup, &QPushButton::clicked, this, [this] { changeLoginStartup(true); });
        connect(ui.removeStartup, &QPushButton::clicked, this, [this] { changeLoginStartup(false); });
        connect(ui.reportsButton, &QPushButton::clicked, this, [this] {
            const QString current = QFileInfo(reportDirectory).canonicalFilePath();
            showReports(this, this->reportsRoot, current, sessionActive() ? current : QString());
            refreshStatistics();
        });
        connect(ui.trialButton, &QPushButton::clicked, this, [this] { startSession(false); });
        connect(ui.runButton, &QPushButton::clicked, this, [this] { startSession(true); });
        connect(stopButton, &QPushButton::clicked, this, [this] { requestStop(); });
        connect(tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
            if (item->parent() && column > 0 && item->checkState(column) == Qt::Checked)
                item->parent()->setCheckState(0, Qt::Checked);
            updateSelection();
        });
        restoreSession();
        refreshStatistics();
        relayNetwork.setProxy(QNetworkProxy::NoProxy);
        auto *heartbeat = new QTimer(this);
        connect(heartbeat, &QTimer::timeout, this, [this] { reportRelayStatus(); });
        heartbeat->start(1000);
        QTimer::singleShot(0, this, [this] { reportRelayStatus(); });
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        if (worker || waiting) {
            event->ignore();
            status->setText("測試仍在執行。請按停止並等待目前的 SDK 或 Relay Agent 操作結束。");
            return;
        }
        if (!sessionActive()) saveSettings();
        QWidget::closeEvent(event);
    }

private:
    QString root, activeFile, reportsRoot, reportDirectory;
    QJsonObject config, session;
    Ui::CardTestWindow ui;
    QTreeWidget *tree = nullptr;
    QTableWidget *results = nullptr;
    QLabel *status = nullptr;
    QPushButton *stopButton = nullptr;
    QCheckBox *keepGoing = nullptr;
    QSpinBox *rounds = nullptr;
    QThread *worker = nullptr;
    std::atomic_bool stop{false};
    bool waiting = false;
    QNetworkAccessManager relayNetwork;
    bool relayReporting = false;
    bool relayReconnecting = false;

    void refreshStatistics()
    {
        ui.statisticsLabel->setText(reportStatistics(reportsRoot, session));
    }
    QString relayCommandUrl, relayCommandId, relayCommandResult;

    QString configuredRelayUrl() const
    {
        return relayFromConfig(sessionActive() ? session.value("settings").toObject() : config).agentUrl;
    }

    void reportRelayStatus()
    {
        if (relayReporting) return;
        const auto options = sessionActive() ? session.value("settings").toObject() : config;
        const auto settings = relayFromConfig(options);
        QString error;
        if (!relayAgentSettingsValid(settings, error)) return;

        const auto phase = session.value("phase").toString();
        const auto outcome = session.value("outcome").toString();
        QString state = "idle";
        if (phase == "running") state = stop ? "stopping" : "running";
        else if (phase == "pending") state = stop ? "stopping" : (waiting || relayReconnecting)
            ? "reconnecting" : "shutdown";
        else if (phase == "uncertain" || outcome == "fail") state = "failed";
        else if (outcome == "pass") state = "passed";
        else if (phase == "finished" && stop) state = "stopped";

        QUrl url(settings.agentUrl);
        url.setPath("/report");
        QUrlQuery query;
        query.addQueryItem("name", QHostInfo::localHostName().left(64));
        query.addQueryItem("state", state);
        if (relayCommandUrl == settings.agentUrl && !relayCommandId.isEmpty()) {
            query.addQueryItem("ack", relayCommandId);
            query.addQueryItem("result", relayCommandResult);
        }
        if (phase == "running" || phase == "pending" || session.contains("completedRounds")) {
            const auto test = session.value("settings").toObject();
            query.addQueryItem("tested", QString::number(session.value("completedRounds").toInt()));
            query.addQueryItem("target", QString::number(test.value("cold").toBool() ? test.value("rounds").toInt() : 1));
        }
        url.setQuery(query);
        QNetworkRequest request(url);
        request.setTransferTimeout(2000);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        relayReporting = true;
        auto *reply = relayNetwork.post(request, QByteArray());
        connect(reply, &QNetworkReply::finished, this, [this, reply, agent = settings.agentUrl] {
            relayReporting = false;
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError || configuredRelayUrl() != agent) return;
            const auto response = QJsonDocument::fromJson(reply->readAll()).object();
            const auto id = response.value("id").toString();
            const auto command = response.value("command").toString();
            if (response.value("channel").toInt() < 1 || id.isEmpty()
                || (command != "start" && command != "stop")
                || (relayCommandUrl == agent && relayCommandId == id)) return;
            relayCommandUrl = agent;
            relayCommandId = id;
            relayCommandResult = "failed";
            if (command == "start") {
                if (!QApplication::activeModalWidget() && startSession(true, true))
                    relayCommandResult = "started";
            } else if (sessionActive() || waiting) {
                requestStop(true);
                relayCommandResult = "stopping";
            }
        });
    }

    void setLamp(const QString &color)
    {
        ui.statusLamp->setStyleSheet("background: " + color + "; border-radius: 6px; margin-top: 0px;");
    }

    bool sessionActive() const
    {
        const auto phase = session.value("phase").toString();
        return phase == "running" || phase == "pending" || phase == "uncertain";
    }

    void updateControls()
    {
        const bool editable = !sessionActive() && !worker && !waiting;
        ui.settingsPanel->setEnabled(editable);
        ui.trialButton->setEnabled(editable);
        ui.runButton->setEnabled(editable);
        ui.reportsButton->setEnabled(!worker && !waiting);
        stopButton->setEnabled(sessionActive() || waiting);
        ui.startupStatus->setText(loginStartupStatus());
    }

    void changeLoginStartup(bool enabled)
    {
        QString error;
        if (!setLoginStartup(enabled, error))
            QMessageBox::warning(this, "登入後自動執行", error);
        ui.startupStatus->setText(loginStartupStatus());
    }

    void saveSettings()
    {
        config["cards"] = selectedCards();
        config.remove("cold");
        config["rounds"] = rounds->value();
        config["keepGoing"] = keepGoing->isChecked();
        if (!saveJson(root + "/settings.json", config))
            status->setText("無法保存測試設定。");
    }

    QJsonArray selectedCards() const
    {
        QJsonArray cards;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            const auto *card = tree->topLevelItem(i);
            if (card->checkState(0) != Qt::Checked) continue;
            QJsonObject item{{"id", card->data(0, identityRole).toString()},
                             {"name", card->data(0, Qt::DisplayRole).toString()}};
            for (int index = 0; index < 2; ++index) {
                QJsonArray checks;
                for (int j = 0; j < card->childCount(); ++j) {
                    const auto *test = card->child(j);
                    if (test->checkState(index + 1) == Qt::Checked)
                        checks.append(test->data(0, checkRole).toString());
                }
                item[QString("ch%1").arg(index)] = checks;
            }
            cards.append(item);
        }
        return cards;
    }

    void updateSelection()
    {
        const auto cards = selectedCards();
        qsizetype checks = 0;
        for (const auto &entry : cards) {
            const auto card = entry.toObject();
            checks += card.value("ch0").toArray().size() + card.value("ch1").toArray().size();
        }
        ui.selectionLabel->setText(QString("已勾選 %1 張卡 · %2 個通道測項").arg(cards.size()).arg(checks));
    }

    void rebuildCards(const QJsonArray &selected)
    {
        const QSignalBlocker blocker(tree);
        tree->clear();
        const auto online = connectedCards();
        QSet<QString> usedNames;
        for (const auto &entry : selected) usedNames.insert(entry.toObject().value("name").toString());
        const auto lookup = [](const QJsonArray &list, const QString &id) {
            for (const auto &item : list) {
                const auto card = item.toObject();
                if (id.compare(card.value("id").toString(), Qt::CaseInsensitive) == 0) return card;
            }
            return QJsonObject{};
        };
        const bool hasReference = QImageReader(config.value("referenceImage").toString()).canRead();
        const auto append = [&](const QJsonObject &device, bool connected) {
            const auto id = device.value("id").toString();
            const auto saved = lookup(selected, id);
            auto name = saved.value("name").toString();
            if (name.isEmpty()) {
                int number = 1;
                do {
                    name = QString("卡 %1 · %2").arg(number++).arg(device.value("name").toString());
                } while (usedNames.contains(name));
                usedNames.insert(name);
            }
            auto *card = new QTreeWidgetItem(tree, {name});
            card->setData(0, identityRole, id);
            card->setToolTip(0, id);
            if (!connected) card->setText(1, "離線");
            card->setCheckState(0, saved.isEmpty() ? Qt::Unchecked : Qt::Checked);
            for (const auto &test : cardChecks()) {
                auto *row = new QTreeWidgetItem(card, {test.label});
                row->setData(0, checkRole, test.id);
                if (!hasReference && (test.id == "color" || test.id == "reference")) {
                    row->setDisabled(true);
                    row->setToolTip(0, "請先在「測項設定」選擇可讀取的參考圖片");
                    continue;
                }
                for (int ch = 0; ch < 2; ++ch)
                    row->setCheckState(ch + 1, saved.value(QString("ch%1").arg(ch)).toArray().contains(test.id)
                        ? Qt::Checked : Qt::Unchecked);
            }
            card->setExpanded(true);
        };
        // Keep the saved labels attached to their paths even when enumeration order changes.
        for (const auto &device : selected)
            append(device.toObject(), !lookup(online, device.toObject().value("id").toString()).isEmpty());
        for (const auto &device : online) {
            if (lookup(selected, device.toObject().value("id").toString()).isEmpty())
                append(device.toObject(), true);
        }
        status->setText(online.isEmpty() ? "目前未偵測到 GVFG 擷取卡。"
            : QString("偵測到 %1 張 GVFG 擷取卡").arg(online.size()));
        setLamp("#64748b");
        updateSelection();
    }

    void copyChecks()
    {
        auto *source = tree->currentItem();
        if (source && source->parent()) source = source->parent();
        if (!source) {
            status->setText("請先在清單中點選一張卡作為來源。");
            return;
        }
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            auto *target = tree->topLevelItem(i);
            if (target == source) continue;
            for (int j = 0; j < source->childCount(); ++j) {
                if (target->child(j)->isDisabled()) continue;
                for (int ch = 1; ch <= 2; ++ch)
                    target->child(j)->setCheckState(ch, source->child(j)->checkState(ch));
            }
            target->setCheckState(0, Qt::Checked);
        }
    }

    void editThresholds()
    {
        QDialog dialog(this);
        dialog.setWindowTitle("測項設定");
        QFormLayout form(&dialog);
        QComboBox interface;
        interface.addItem("任意", "");
        interface.addItem("SDI", "SDI");
        interface.addItem("HDMI", "HDMI");
        interface.setCurrentIndex(qMax(0, interface.findData(config.value("interface").toString())));
        form.addRow("輸入介面", &interface);
        QSpinBox width, height, depth, signal, sample, rate, channels, bootWait, freeze;
        QDoubleSpinBox fps, colorLimit, referenceLimit;
        const auto integer = [&](QSpinBox &box, const QString &label, const QString &key, int min, int max, int initial) {
            box.setRange(min, max);
            box.setValue(config.value(key).toInt(initial));
            if (!min) box.setSpecialValueText("任意");
            form.addRow(label, &box);
        };
        integer(width, "寬度", "width", 0, 16384, 1920);
        integer(height, "高度", "height", 0, 16384, 1080);
        integer(depth, "位元深度", "depth", 0, 16, 0);
        integer(signal, "訊號等待（秒）", "signalSeconds", 1, 120, 10);
        integer(sample, "擷取取樣（秒）", "sampleSeconds", 1, 300, 5);
        integer(rate, "PCM 取樣率（Hz）", "sampleRate", 0, 192000, 48000);
        integer(channels, "PCM 聲道數", "audioChannels", 0, 32, 2);
        integer(bootWait, "登入後等待（秒）", "bootWait", 1, 300, 15);
        fps.setRange(1, 240);
        fps.setValue(config.value("minFps").toDouble(25));
        form.addRow("最低 FPS", &fps);
        integer(freeze, "凍結門檻（毫秒）", "freezeMs", 100, 300000, 1000);
        QLineEdit reference(config.value("referenceImage").toString());
        QPushButton browse("選擇圖片…");
        connect(&browse, &QPushButton::clicked, &dialog, [&] {
            const auto path = QFileDialog::getOpenFileName(&dialog, "選擇與輸入畫面相同的參考圖片",
                reference.text(), "圖片 (*.png *.bmp *.jpg *.jpeg)");
            if (!path.isEmpty()) reference.setText(path);
        });
        form.addRow("固定來源畫面的參考圖片", &reference);
        form.addRow(&browse);
        for (auto *limit : {&colorLimit, &referenceLimit}) limit->setRange(0, 255);
        colorLimit.setValue(config.value("colorLimit").toDouble(15));
        referenceLimit.setValue(config.value("referenceLimit").toDouble(15));
        form.addRow("色彩差異上限", &colorLimit);
        form.addRow("畫面差異上限", &referenceLimit);
        QCheckBox saveEvidence("影像測項失敗時保留畫面");
        saveEvidence.setChecked(config.value("saveEvidence").toBool());
        form.addRow(&saveEvidence);
        QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        form.addRow(&buttons);
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return;
        config["interface"] = interface.currentData().toString();
        config["width"] = width.value();
        config["height"] = height.value();
        config["depth"] = depth.value();
        config["signalSeconds"] = signal.value();
        config["sampleSeconds"] = sample.value();
        config["sampleRate"] = rate.value();
        config["audioChannels"] = channels.value();
        config["bootWait"] = bootWait.value();
        config["minFps"] = fps.value();
        config["freezeMs"] = freeze.value();
        config["referenceImage"] = reference.text().trimmed();
        rebuildCards(selectedCards());
        config["colorLimit"] = colorLimit.value();
        config["referenceLimit"] = referenceLimit.value();
        config["saveEvidence"] = saveEvidence.isChecked();
        saveSettings();
    }

    void editAgent()
    {
        QDialog dialog(this);
        dialog.setWindowTitle("Relay Agent 設定");
        QFormLayout form(&dialog);
        const auto existing = relayFromConfig(config);
        QLineEdit address(existing.agentUrl);
        address.setPlaceholderText("http://192.168.50.20:8080");
        QSpinBox delay;
        delay.setRange(15, 3600);
        delay.setValue(existing.delaySeconds);
        QDoubleSpinBox duration;
        duration.setDecimals(1);
        duration.setRange(0.1, 6553.5);
        duration.setValue(existing.ticks / 10.0);
        QPushButton probe("測試連線");
        QLabel message;
        message.setWordWrap(true);
        QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        form.addRow("Agent 網址", &address);
        form.addRow("關機緩衝（秒）", &delay);
        form.addRow("斷電時間（秒）", &duration);
        form.addRow(&probe);
        form.addRow(&message);
        form.addRow(&buttons);
        dialog.resize(470, dialog.sizeHint().height());
        const auto current = [&] {
            RelaySettings settings;
            settings.agentUrl = address.text().trimmed();
            settings.delaySeconds = delay.value();
            settings.ticks = qRound(duration.value() * 10);
            return settings;
        };
        connect(&probe, &QPushButton::clicked, &dialog, [&] {
            probe.setEnabled(false);
            int channel = 0;
            QString error;
            if (checkRelayAgent(current(), channel, error, true))
                message.setText(QString("連線成功，Agent 使用繼電器通道 %1。").arg(channel));
            else message.setText(error);
            probe.setEnabled(true);
        });
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            QString error;
            if (relayAgentSettingsValid(current(), error)) dialog.accept();
            else message.setText(error);
        });
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return;
        const auto settings = current();
        config["agent"] = settings.agentUrl;
        config["shutdownDelay"] = settings.delaySeconds;
        config["offTicks"] = settings.ticks;
        saveSettings();
    }

    QJsonObject runConfig() const
    {
        QJsonObject options{{"width", 1920}, {"height", 1080}, {"signalSeconds", 10},
            {"sampleSeconds", 5}, {"sampleRate", 48000}, {"audioChannels", 2}, {"bootWait", 15},
            {"minFps", 25.0}};
        for (auto it = config.begin(); it != config.end(); ++it) options.insert(it.key(), it.value());
        return options;
    }

    bool store()
    {
        return saveJson(activeFile, session);
    }

    void finish(const QString &message)
    {
        waiting = false;
        const bool startup = session.value("settings").toObject().value("cold").toBool();
        session["phase"] = "finished";
        session["ending"] = message;
        session["finishedAt"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
        const bool saved = store();
        const bool archived = saveJson(reportDirectory + "/session.json", session);
        QString error;
        const bool removed = !startup || setResumeStartup(false, error);
        status->setText(message + (saved && archived ? QString() : "。無法保存完整的結束狀態。")
            + (removed ? QString() : "。" + error));
        const QString outcome = session.value("outcome").toString();
        setLamp(outcome == "pass" && saved && archived && removed ? "#15803d"
            : outcome == "fail" || !saved || !archived || !removed ? "#b91c1c" : "#64748b");
        ui.progressLabel->clear();
        updateControls();
        refreshStatistics();
    }

    void requireRelayReview(const QString &message)
    {
        session["phase"] = "uncertain";
        session["relayReviewReason"] = message;
        const bool saved = store();
        QString error;
        const bool removed = setResumeStartup(false, error);
        status->setText(message + (saved ? QString() : " 無法保存排程狀態。")
            + (removed ? QString() : " " + error));
        setLamp("#b45309");
        updateControls();
    }

    void restoreSession()
    {
        session = loadJson(activeFile);
        if (session.isEmpty()) {
            updateControls();
            return;
        }
        const QString id = session.value("id").toString();
        if (QUuid(id).isNull()) {
            session = {};
            updateControls();
            return;
        }
        reportDirectory = QDir(reportsRoot).filePath(reportCategory(session.value("settings").toObject())
            + '/' + QUuid(id).toString(QUuid::WithoutBraces));
        if (!sessionActive()) {
            status->setText(session.value("ending").toString());
            const auto outcome = session.value("outcome").toString();
            if (outcome == "pass") setLamp("#15803d");
            else if (outcome == "fail") setLamp("#b91c1c");
            updateControls();
            return;
        }
        const auto options = session.value("settings").toObject();
        rebuildCards(options.value("cards").toArray());
        const QString phase = session.value("phase").toString();
        const int round = session.value("round").toInt();
        if (phase == "running") {
            finish(roundName(round) + "上次執行中斷，請重新開始測試。");
            return;
        }
        QString blocked;
        if (phase == "uncertain" || session.value("stopRequested").toBool())
            blocked = session.value("relayReviewReason").toString("上次排程或停止結果尚未確認。");
        else if (QUuid(session.value("request").toString()).isNull())
            blocked = "找不到上一輪的 Relay Agent 排程識別碼。";
        if (!blocked.isEmpty()) {
            status->setText(blocked + " 按停止可嘗試取消排程。");
            setLamp("#b45309");
            updateControls();
            return;
        }
        const int seconds = qBound(1, options.value("bootWait").toInt(15), 300);
        waiting = true;
        stop = false;
        status->setText(QString("%1 秒後確認 Relay Agent 上一輪排程，確認後執行%2。")
            .arg(seconds).arg(roundName(round)));
        setLamp("#b45309");
        updateControls();
        QTimer::singleShot(seconds * 1000, this, [this] {
            if (!waiting) return;
            waiting = false;
            confirmRelayAndResume();
        });
    }

    void confirmRelayAndResume()
    {
        const auto relay = relayFromConfig(session.value("settings").toObject());
        const auto request = session.value("request").toString();
        struct Verification { QString state, error; };
        auto confirmation = std::make_shared<Verification>();
        status->setText("正在向 Relay Agent 確認上次斷電排程，最多重試 60 秒。");
        launch([this, relay, request, confirmation] {
            QElapsedTimer retry;
            retry.start();
            while (!stop && retry.elapsed() < 60000) {
                confirmation->state = relayAgentRequestState(relay, request, confirmation->error);
                if (confirmation->state == "sent" || confirmation->state == "failed"
                    || confirmation->state == "cancelled") return;
                for (int tick = 0; tick < 10 && !stop; ++tick) QThread::msleep(100);
            }
        }, [this, confirmation] {
            if (confirmation->state == "sent") {
                if (stop) finish("已停止，未開始下一輪。");
                else runRound();
            } else if (confirmation->state == "failed" || confirmation->state == "cancelled") {
                finish("Relay Agent 未完成本次斷電排程（" + confirmation->state + "）。");
            } else if (stop) {
                requestStop();
            } else {
                requireRelayReview("無法確認 Relay Agent 已送出斷電指令（"
                    + (confirmation->error.isEmpty() ? confirmation->state : confirmation->error)
                    + "）。按停止可嘗試取消排程。");
            }
        });
    }

    bool startSession(bool cold, bool remote = false)
    {
        if (sessionActive() || worker || waiting) return false;
        if (selectedCards().isEmpty()) {
            status->setText("請勾選至少一張卡或一個通道測項。");
            return false;
        }
        saveSettings();
        auto options = runConfig();
        for (const auto &entry : options.value("cards").toArray()) {
            const auto card = entry.toObject();
            for (const auto &key : {"ch0", "ch1"}) {
                const auto checks = card.value(key).toArray();
                if ((checks.contains("color") || checks.contains("reference"))
                    && !QImageReader(options.value("referenceImage").toString()).canRead()) {
                    status->setText("請先在測項設定選擇可讀取的參考圖片。");
                    return false;
                }
                if (checks.contains("freeze") && options.value("sampleSeconds").toInt() * 1000
                        <= options.value("freezeMs").toInt(1000)) {
                    status->setText("影像取樣時間必須長於凍結門檻。");
                    return false;
                }
            }
        }
        options["cold"] = cold;
        if (cold) {
            QString error;
            if (!relayAgentSettingsValid(relayFromConfig(options), error)) {
                status->setText(error);
                return false;
            }
            if (!remote && QMessageBox::question(this, "開始冷開機測試",
                QString("第 1 輪先在本次開機執行。輪次之間會正常關閉 Windows，由 Relay Agent 斷電後復電。"
                        "\n%1。請先儲存其他工作並確認 BIOS 來電自動開機及 Windows 自動登入已設定。")
                    .arg(rounds->value() ? QString("預計 %1 輪").arg(rounds->value()) : "輪數不限"),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return false;
        }

        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        reportDirectory = QDir(reportsRoot).filePath(reportCategory(options) + '/' + id);
        if (!QDir().mkpath(reportDirectory) || !saveJson(reportDirectory + "/settings.json", options)) {
            status->setText("無法建立測試紀錄資料夾。");
            return false;
        }
        session = {{"id", id}, {"settings", options}, {"round", 1},
            {"startedAt", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}};
        stop = false;
        runRound();
        return session.value("phase").toString() == "running";
    }

    void launch(std::function<void()> task, std::function<void()> done)
    {
        worker = QThread::create(std::move(task));
        connect(worker, &QThread::finished, this, [this, done = std::move(done)] {
            worker->deleteLater();
            worker = nullptr;
            done();
        });
        updateControls();
        worker->start();
    }

    void showResult(int index, const QJsonObject &row)
    {
        if (index == results->rowCount()) results->insertRow(index);
            const int channel = row.value("channel").toInt();
            const QStringList values{row.value("card").toString(), channel < 0 ? QString() : QString("CH%1").arg(channel),
                row.value("check").toString(), row.value("result").toString(), row.value("detail").toString()};
            for (int column = 0; column < values.size(); ++column) {
                auto *cell = new QTableWidgetItem(values[column]);
                if (column == 3) {
                    const bool passed = values[column] == "PASS";
                    const bool failed = values[column] == "FAIL";
                    const bool running = values[column] == "測試中";
                    cell->setBackground(QColor(passed ? "#dcfce7" : failed ? "#fee2e2" : running ? "#dbeafe" : "#f1f5f9"));
                    cell->setForeground(QColor(passed ? "#166534" : failed ? "#991b1b" : running ? "#1d4ed8" : "#334155"));
                }
                if (column == 0) cell->setData(Qt::UserRole, QVariant::fromValue(row));
                cell->setToolTip(column == 0 ? row.value("device").toString() : values[column]);
                results->setItem(index, column, cell);
            }
    }

    void updateLiveResult(const QJsonObject &row)
    {
        int index = 0;
        for (; index < results->rowCount(); ++index) {
            const auto *item = results->item(index, 0);
            if (!item) continue;
            const auto previous = item->data(Qt::UserRole).value<QJsonObject>();
            if (previous.value("device") == row.value("device") && previous.value("channel") == row.value("channel")
                && previous.value("check") == row.value("check")) break;
        }
        showResult(index, row);
        results->scrollToItem(results->item(index, 3));
        for (int column = 0; column < 4; ++column) results->resizeColumnToContents(column);
    }

    void showResults(const QJsonArray &rows)
    {
        results->setRowCount(0);
        for (const auto &entry : rows) showResult(results->rowCount(), entry.toObject());
        for (int column = 0; column < 4; ++column) results->resizeColumnToContents(column);
    }

    void runRound()
    {
        session["phase"] = "running";
        session["lastStartedRound"] = session.value("round");
        session.remove("request");
        if (!store()) {
            finish("無法保存測試進度。");
            return;
        }
        refreshStatistics();
        const auto options = session.value("settings").toObject();
        const int round = session.value("round").toInt();
        results->setRowCount(0);
        ui.diagnosticsLog->clear();
        const int limit = options.value("cold").toBool() ? options.value("rounds").toInt() : 1;
        status->setText(limit ? QString("測試中 · 第 %1／%2 輪").arg(round).arg(limit)
            : QString("測試中 · 第 %1 輪（不限）").arg(round));
        setLamp("#2563eb");
        ui.progressLabel->setText("準備檢查…");
        ui.roundProgress->setRange(0, 0);
        ui.roundProgress->show();
        auto rows = std::make_shared<QJsonArray>();
        auto logOk = std::make_shared<bool>(true);
        const QString logPath = QDir(reportDirectory).filePath(roundLogName(round));
        auto workerError = std::make_shared<QString>();
        launch([this, options, rows, logOk, logPath, workerError] {
            QFile log(logPath);
            *logOk = log.open(QIODevice::WriteOnly | QIODevice::Text);
            const auto writeLog = [this, &log, logOk](const QString &message) {
                const auto line = QDateTime::currentDateTime().toString(Qt::ISODateWithMs) + " · " + message;
                const auto bytes = (line + '\n').toUtf8();
                if (*logOk) *logOk = log.write(bytes) == bytes.size() && log.flush();
                QMetaObject::invokeMethod(this, [this, line] {
                    ui.diagnosticsLog->appendPlainText(line);
                }, Qt::QueuedConnection);
            };
            QTemporaryDir temporary;
            auto workerOptions = options;
            workerOptions.remove("evidenceDirectory");
            if (options.value("saveEvidence").toBool())
                workerOptions["evidenceDirectory"] = QFileInfo(logPath).absolutePath() + "/"
                    + QFileInfo(logPath).completeBaseName() + "_images";
            if (!temporary.isValid() || !saveJson(temporary.filePath("options.json"), workerOptions)) {
                *workerError = "無法建立擷取子程序的測試設定";
                writeLog(*workerError);
                return;
            }
            QProcess process;
            process.setStandardErrorFile(QProcess::nullDevice());
            process.start(QCoreApplication::applicationFilePath(), {"--capture-worker", temporary.path()});
            if (!process.waitForStarted(5000)) {
                *workerError = "無法啟動擷取子程序：" + process.errorString();
                writeLog(*workerError);
                return;
            }
            writeLog(QString("擷取子程序已啟動 · PID %1").arg(process.processId()));
            QString lastStep;
            const auto drain = [&] {
                while (process.canReadLine()) {
                    const auto msg = QJsonDocument::fromJson(process.readLine()).object();
                    if (msg.value("row").isObject()) {
                        const auto row = msg.value("row").toObject();
                        if (row.value("result").toString() != "測試中") rows->append(row);
                        QMetaObject::invokeMethod(this, [this, row] { updateLiveResult(row); }, Qt::QueuedConnection);
                    } else if (msg.contains("step")) {
                        lastStep = msg.value("step").toString();
                        writeLog(lastStep);
                        QMetaObject::invokeMethod(this, [this, step = lastStep] {
                            if (!stop) ui.progressLabel->setText(step);
                        }, Qt::QueuedConnection);
                    } else if (msg.contains("log")) {
                        writeLog(msg.value("log").toString());
                    } else if (msg.contains("total")) {
                        const int done = msg.value("done").toInt(), total = msg.value("total").toInt();
                        QMetaObject::invokeMethod(this, [this, done, total] {
                            ui.roundProgress->setRange(0, total);
                            ui.roundProgress->setValue(done);
                        }, Qt::QueuedConnection);
                    }
                }
            };
            QElapsedTimer stopping;
            while (process.state() != QProcess::NotRunning) {
                process.waitForReadyRead(100);
                drain();
                if (stop && !stopping.isValid()) {
                    QFile marker(temporary.filePath("stop"));
                    if (!marker.open(QIODevice::WriteOnly)) writeLog("無法通知擷取子程序停止。");
                    stopping.start();
                }
                if (stopping.isValid() && stopping.elapsed() >= 5000
                    && process.state() != QProcess::NotRunning) {
                    writeLog("停止等待逾時，已終止擷取子程序。");
                    process.kill();
                    process.waitForFinished(3000);
                    break;
                }
            }
            drain();
            const auto result = loadJson(temporary.filePath("result.json"));
            if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0
                && result.value("rows").isArray()) {
                *rows = result.value("rows").toArray();
                return;
            }
            if (stop) return;
            *workerError = QString("擷取子程序異常結束（0x%1）").arg(quint32(process.exitCode()), 8, 16, QChar('0'));
            if (!lastStep.isEmpty()) *workerError += "，最後步驟：" + lastStep;
            writeLog(*workerError);
        }, [this, options, round, rows, logOk, workerError] {
            if (!workerError->isEmpty())
                rows->append(QJsonObject{{"channel", -1}, {"check", "擷取子程序"},
                    {"result", "FAIL"}, {"detail", *workerError}});
            showResults(*rows);
            if (!writeRoundReport(reportDirectory, round, *rows, stop)) {
                finish("無法保存本輪報告。");
                return;
            }
            refreshStatistics();
            if (!*logOk) {
                session["outcome"] = "fail";
                finish("無法完整保存本輪 SDK 紀錄。");
                return;
            }
            if (!workerError->isEmpty()) {
                session["outcome"] = "fail";
                finish(*workerError);
                return;
            }
            if (stop) {
                finish(roundName(round) + "已停止。");
                return;
            }
            const bool passed = roundPassed(*rows);
            session["completedRounds"] = round;
            if (!passed) session["failedRounds"] = session.value("failedRounds").toInt() + 1;
            const int limit = options.value("cold").toBool() ? options.value("rounds").toInt() : 1;
            if ((limit > 0 && round >= limit) || (!passed && !options.value("keepGoing").toBool())) {
                const int failures = session.value("failedRounds").toInt();
                session["outcome"] = failures ? "fail" : "pass";
                finish(limit ? QString("完成 %1／%2 輪，%3 輪未通過。").arg(round).arg(limit).arg(failures)
                    : QString("完成 %1 輪，%2 輪未通過。").arg(round).arg(failures));
                return;
            }
            queueColdBoot(round, options);
        });
    }

    void queueColdBoot(int round, const QJsonObject &options)
    {
        QString error;
        if (!setResumeStartup(true, error)) {
            finish(error);
            return;
        }
        const auto request = QUuid::createUuid().toString(QUuid::WithoutBraces);
        session["round"] = round + 1;
        session["request"] = request;
        session["phase"] = "pending";
        if (!store()) {
            finish("無法保存冷開機續跑進度。");
            return;
        }
        status->setText("正在確認 Relay Agent，排程成功後將正常關機。");
        setLamp("#b45309");
        ui.progressLabel->clear();
        const auto relay = relayFromConfig(options);
        struct Outcome { QString error; bool pending = false; };
        auto outcome = std::make_shared<Outcome>();
        const QString logPath = QDir(reportDirectory).filePath(roundLogName(round));
        launch([this, relay, request, outcome, logPath] {
            outcome->error = scheduleShutdown(relay, request, stop, outcome->pending,
                [this, logPath](const QString &message, bool reconnecting) {
                    const auto line = QDateTime::currentDateTime().toString(Qt::ISODateWithMs) + " · " + message;
                    QFile file(logPath);
                    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
                        file.write((line + '\n').toUtf8());
                    QMetaObject::invokeMethod(this, [this, line, message, reconnecting] {
                        relayReconnecting = reconnecting;
                        if (!stop) status->setText(message);
                        ui.diagnosticsLog->appendPlainText(line);
                    }, Qt::QueuedConnection);
                });
        }, [this, outcome] {
            relayReconnecting = false;
            if (outcome->error.isEmpty()) {
                status->setText("已要求 Windows 關機，復電並登入後續跑。");
                stopButton->setEnabled(false);
                return;
            }
            if (outcome->pending)
                requireRelayReview(outcome->error);
            else
                finish(outcome->error);
        });
    }

    void requestStop(bool remote = false)
    {
        stop = true;
        if (sessionActive() && session.value("phase").toString() != "running") {
            session["stopRequested"] = true;
            store();
        }
        if (worker) {
            status->setText("正在停止，等待目前操作完成…");
            setLamp("#b45309");
            return;
        }
        if (waiting) {
            waiting = false;
        }
        if (!sessionActive()) return;
        const auto relay = relayFromConfig(session.value("settings").toObject());
        const auto request = session.value("request").toString();
        const auto error = std::make_shared<QString>();
        launch([relay, request, error] { cancelRelayAgent(relay, request, *error); }, [this, error, remote] {
            if (error->isEmpty()) finish("已取消 Relay Agent 排程並停止測試。");
            else {
                // Keep the journal until the operator checks a possibly accepted request.
                status->setText("無法確認已取消：" + *error);
                updateControls();
                if (!remote && QMessageBox::question(this, "停止續跑",
                        "請在 Relay Agent 端確認排程已停止或已結束。確認後要結束此測試並移除自動續跑嗎？",
                        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
                    finish("使用者已確認控制端排程，測試停止。");
            }
        });
    }
};

bool relayClientSelfTest()
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) return false;
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    int posts = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        auto request = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, request] {
            request->append(socket->readAll());
            if (!request->contains("\r\n\r\n")) return;
            const auto line = request->left(request->indexOf("\r\n")).split(' ');
            const QUrl url(QString::fromLatin1(line.value(1)));
            const QUrlQuery query(url);
            QByteArray body;
            if (line.value(0) == "GET" && url.path() == "/health") {
                body = R"({"state":"online","channel":1,"serialReady":true})";
            } else if (url.path() == "/power-cycle" && query.queryItemValue("id") == id) {
                if (line.value(0) == "POST" && query.queryItemValue("delay") == "30"
                    && query.queryItemValue("ticks") == "200") {
                    ++posts;
                    body = "{\"id\":\"" + id.toLatin1() + "\",\"state\":\"scheduled\",\"remainingMs\":30000}";
                } else if (!query.hasQueryItem("delay") && !query.hasQueryItem("ticks")) {
                    if (line.value(0) == "GET")
                        body = "{\"id\":\"" + id.toLatin1() + "\",\"state\":\"sent\"}";
                    else if (line.value(0) == "DELETE")
                        body = "{\"id\":\"" + id.toLatin1() + "\",\"state\":\"cancelled\"}";
                }
            }
            const QByteArray response = body.isEmpty() ? "HTTP/1.1 400 Bad Request\r\n" : "HTTP/1.1 200 OK\r\n";
            socket->write(response + "Content-Type: application/json\r\nConnection: close\r\nContent-Length: "
                + QByteArray::number(body.size()) + "\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });
    RelaySettings relay;
    relay.agentUrl = QString("http://127.0.0.1:%1").arg(server.serverPort());
    const std::atomic_bool stopped{true};
    bool pending = false;
    if (scheduleShutdown(relay, id, stopped, pending) != "已停止" || pending || posts != 0)
        return false;
    auto request = id;
    QString error;
    bool stoppedByAgent = false;
    return scheduleRelayAgent(relay, request, error, stoppedByAgent) && posts == 1
        && relayAgentRequestState(relay, id, error) == "sent" && cancelRelayAgent(relay, id, error);
}

int selfTest()
{
    if (!imageSamplesSelfTest()) return 1;
    if (relayFromConfig({}).agentUrl != "http://192.168.50.20:8080"
        || relayFromConfig({{"agent", "http://192.168.50.21:8080"}}).agentUrl != "http://192.168.50.21:8080")
        return 1;
    QTemporaryDir directory;
    const QJsonArray cards{QJsonObject{{"name", "測試卡"}, {"id", "missing-fixture-path"},
        {"ch0", QJsonArray{"open", "video"}}, {"ch1", QJsonArray{"audio"}}},
        QJsonObject{{"name", "測試卡"}, {"id", "second-missing-fixture-path"},
            {"ch1", QJsonArray{"signal", "video", "video8", "video10", "audio", "peak", "continuity"}}}};
    const std::atomic_bool stop{false};
    const auto rows = checkCards(cards, {}, stop);
    const auto passed = QJsonArray{QJsonObject{{"result", "PASS"}}};
    const QString path = directory.filePath("session.json");
    if (!directory.isValid() || rows.size() != 12
        || rows.at(6).toObject().value("check").toString() != "8-bit · 影像擷取與 FPS"
        || rows.at(8).toObject().value("check").toString() != "8-bit · 取樣期間不得斷訊"
        || rows.at(9).toObject().value("check").toString() != "10-bit · 影像擷取與 FPS"
        || rows.at(11).toObject().value("check").toString() != "10-bit · 取樣期間不得斷訊"
        || roundPassed(rows) || !roundPassed(passed)
        || rows.first().toObject().value("result").toString() != "FAIL"
        || rows.last().toObject().value("result").toString() != "未執行"
        || !saveJson(path, {{"cards", cards}}) || loadJson(path).value("cards").toArray() != cards
        || !writeRoundReport(directory.path(), 1, rows, false)) return 1;
    QFile csv(directory.filePath("round_0001.csv"));
    if (!csv.open(QIODevice::ReadOnly)) return 1;
    const auto data = csv.readAll();
    return data.contains("\"missing-fixture-path\"") && data.contains("\"second-missing-fixture-path\"")
        && reportSelfTest(directory.path()) && relayClientSelfTest() ? 0 : 1;
}
}

int main(int argc, char **argv)
{
    if (argc == 3 && QByteArray(argv[1]) == "--capture-worker") {
        QCoreApplication application(argc, argv);
        return runCaptureWorker(application, application.arguments().at(2));
    }
    if (argc == 2 && QByteArray(argv[1]) == "--self-test") {
        QCoreApplication application(argc, argv);
        return selfTest();
    }
    QApplication application(argc, argv);
    application.setApplicationName("GVFG_CardTest");
    application.setOrganizationName("GVFG");
    if (application.arguments().contains("--ui-self-test")) {
        QTemporaryDir directory;
        QTcpServer agent;
        if (!agent.listen(QHostAddress::LocalHost)) return 1;
        bool reportedFailure = false;
        QObject::connect(&agent, &QTcpServer::newConnection, &agent, [&] {
            auto *socket = agent.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                if (!socket->canReadLine()) return;
                const auto request = socket->readLine();
                reportedFailure |= request.startsWith("POST /report?")
                    && request.contains("state=failed") && request.contains("tested=1") && request.contains("target=1");
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
                socket->disconnectFromHost();
            });
        });
        const QJsonArray cards{QJsonObject{{"id", "missing-ui-fixture"}, {"name", "試跑測試卡"},
            {"ch0", QJsonArray{"open"}}}};
        if (!directory.isValid() || !saveJson(directory.filePath("settings.json"), {{"cards", cards},
                {"agent", QString("http://127.0.0.1:%1").arg(agent.serverPort())}}))
            return 1;
        const auto reports = directory.filePath("reports");
        const auto legacy = directory.filePath("legacy");
        const auto legacyId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QDir().mkpath(legacy + '/' + legacyId)
            || !saveJson(legacy + '/' + legacyId + "/settings.json", {{"cold", true}})
            || !prepareReports(legacy, reports)
            || QFileInfo::exists(legacy + '/' + legacyId)
            || !QFileInfo::exists(reports + "/cold/" + legacyId + "/settings.json")) return 1;
        CardTestWindow window(directory.path(), reports);
        auto *trial = window.findChild<QPushButton *>("trialButton");
        auto *formal = window.findChild<QPushButton *>("runButton");
        auto *reportButton = window.findChild<QPushButton *>("reportsButton");
        auto *results = window.findChild<QTableWidget *>("resultsTable");
        if (!trial || !formal || !reportButton || !results || !trial->isEnabled()) return 1;
        window.show();
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &window, [&] {
            if (!reportedFailure || !trial->isEnabled() || results->rowCount() != 2) return;
            poll.stop();
            const auto state = loadJson(directory.filePath("active.json"));
            const QString report = directory.filePath("reports/trial/" + state.value("id").toString() + "/round_0001.csv");
            const bool trialOk = state.value("phase").toString() == "finished"
                && !state.value("settings").toObject().value("cold").toBool()
                && results->item(0, 3)->text() == "FAIL" && QFile::exists(report);
            bool reportOk = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = window.findChild<QDialog *>("reportDialog");
                if (!dialog) return;
                auto *table = dialog->findChild<QTableWidget *>("reportTable");
                auto *diagnostics = dialog->findChild<QPlainTextEdit *>("reportDiagnostics");
                reportOk = table && table->rowCount() == 2 && table->item(0, 6)
                    && table->item(0, 6)->text() == "FAIL" && !table->item(0, 6)->icon().isNull()
                    && diagnostics && diagnostics->toPlainText().contains("警告 · 找不到裝置介面");
                dialog->accept();
            });
            reportButton->click();
            application.exit(trialOk && reportOk ? 0 : 1);
        });
        poll.start(20);
        QTimer::singleShot(0, trial, &QPushButton::click);
        QTimer::singleShot(10000, &window, [&] { application.exit(1); });
        return application.exec();
    }
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty() || !QDir().mkpath(root)) {
        QMessageBox::critical(nullptr, "GVFG 擷取卡測試", "無法建立本機測試資料夾。");
        return 1;
    }
    QLockFile lock(root + "/instance.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) return 1;
    if (application.arguments().contains("--resume")) {
        const auto phase = loadJson(root + "/active.json").value("phase").toString();
        if (phase.isEmpty() || phase == "finished") return 0;
    }
    const QString reports = QCoreApplication::applicationDirPath() + "/reports";
    if (!prepareReports(root + "/reports", reports)) {
        QMessageBox::critical(nullptr, "GVFG 擷取卡測試",
            "無法建立報告資料夾或移動舊報告。請確認程式資料夾可寫入，且目的地沒有同名報告。\n" + reports);
        return 1;
    }
    CardTestWindow window(root, reports);
    window.show();
    return application.exec();
}
