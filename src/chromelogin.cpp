#include "chromelogin.h"

#include "paths.h"
#include "pixiv.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>

namespace {
const char* kLoginUrl = "https://accounts.pixiv.net/login?return_to=https%3A%2F%2Fwww.pixiv.net%2F";
constexpr qint64 kHandoffMs = 3000;  // これより早く終わったら、既に開いているブラウザに引き渡されたとみなす
constexpr qint64 kReadTimeoutMs = 30000;
}  // namespace

ChromeLogin::ChromeLogin(QObject* parent) : QObject(parent)
{
    connect(&portTimer_, &QTimer::timeout, this, &ChromeLogin::checkPort);
    connect(&pollTimer_, &QTimer::timeout, this, &ChromeLogin::poll);
    connect(&ws_, &CdpSocket::connected, this, [this] {
        poll();
        pollTimer_.start(1000);
    });
    connect(&ws_, &CdpSocket::textMessageReceived, this, &ChromeLogin::onMessage);
}

QString ChromeLogin::findBrowser()
{
    const QString pf = qEnvironmentVariable("ProgramFiles"), pf86 = qEnvironmentVariable("ProgramFiles(x86)"),
                  local = qEnvironmentVariable("LOCALAPPDATA");
    const QStringList candidates = {
        pf + "/Google/Chrome/Application/chrome.exe",
        pf86 + "/Google/Chrome/Application/chrome.exe",
        local + "/Google/Chrome/Application/chrome.exe",
        pf86 + "/Microsoft/Edge/Application/msedge.exe",
        pf + "/Microsoft/Edge/Application/msedge.exe",
    };
    for (const auto& c : candidates)
        if (QFileInfo::exists(c))
            return c;
    return {};
}

QProcess* ChromeLogin::launch(const QStringList& args)
{
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, p, &QObject::deleteLater);
    p->start(exe_, QStringList{"--user-data-dir=" + QDir::toNativeSeparators(profile_), "--no-first-run",
                               "--no-default-browser-check"} + args);
    return p;
}

void ChromeLogin::start()
{
    if (phase_ != Idle)
        return;
    exe_ = findBrowser();
    if (exe_.isEmpty()) {
        emit finished(false, "Google Chrome または Microsoft Edge が見つかりません");
        return;
    }
    profile_ = paths::profileDir();
    QDir().mkpath(profile_);

    phase_ = Interactive;
    elapsed_.start();
    proc_ = launch({"--new-window", kLoginUrl});
    connect(proc_, &QProcess::finished, this, [this] {
        if (phase_ != Interactive)
            return;
        if (elapsed_.elapsed() < kHandoffMs)
            finish(false, "ログイン用のブラウザが既に開いているようです。そのブラウザでログインしてから閉じ、もう一度「ログイン」を押してください");
        else
            startReading();
    });
    emit status("開いたブラウザで pixiv にログインし、終わったらブラウザを閉じてください");
}

// 閉じられたプロファイルを画面なしで開き直し、DevTools から Cookie を読む
void ChromeLogin::startReading()
{
    phase_ = Reading;
    polls_ = 0;
    userAgent_.clear();
    QFile::remove(profile_ + "/DevToolsActivePort");
    elapsed_.start();
    proc_ = launch({"--headless=new", "--remote-debugging-port=0", "about:blank"});
    connect(proc_, &QProcess::finished, this, [this] {
        if (phase_ == Reading)
            finish(false, "ログイン情報を読み取れませんでした。もう一度お試しください");
    });
    portTimer_.start(300);
    emit status("ログイン情報を確認しています…");
}

// Chrome は起動するとプロファイルに DevToolsActivePort (1行目: ポート, 2行目: パス) を書く
void ChromeLogin::checkPort()
{
    QFile f(profile_ + "/DevToolsActivePort");
    if (!f.open(QIODevice::ReadOnly)) {
        if (elapsed_.elapsed() > kReadTimeoutMs)
            finish(false, "ブラウザとの接続を開始できませんでした");
        return;
    }
    const QStringList lines = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
    if (lines.size() < 2)
        return;
    portTimer_.stop();
    const QString port = lines[0].trimmed(), path = lines[1].trimmed();

    QNetworkReply* reply = nam_.get(QNetworkRequest(QUrl("http://127.0.0.1:" + port + "/json/version")));
    connect(reply, &QNetworkReply::finished, this, [this, reply, port, path] {
        reply->deleteLater();
        if (phase_ != Reading)
            return;
        const QJsonObject v = QJsonDocument::fromJson(reply->readAll()).object();
        // 画面なしの Chrome は "HeadlessChrome" を名乗るので、普通の Chrome の表記に直す
        userAgent_ = v["User-Agent"].toString().replace("HeadlessChrome", "Chrome");
        QString ws = v["webSocketDebuggerUrl"].toString();
        if (ws.isEmpty())
            ws = "ws://127.0.0.1:" + port + path;
        ws_.open(QUrl(ws));
    });
}

void ChromeLogin::send(const QString& method)
{
    const QJsonObject msg{{"id", ++nextId_}, {"method", method}};
    ws_.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

void ChromeLogin::poll()
{
    if (elapsed_.elapsed() > kReadTimeoutMs) {
        finish(false, "ログイン情報の確認に時間がかかりすぎました。もう一度お試しください");
        return;
    }
    send("Storage.getCookies");
    cookieReq_ = nextId_;
}

void ChromeLogin::onMessage(const QString& text)
{
    const QJsonObject o = QJsonDocument::fromJson(text.toUtf8()).object();
    if (phase_ != Reading || o["id"].toInt() != cookieReq_)
        return;
    QJsonArray pixivCookies;
    QString uid;
    static const QRegularExpression loggedIn(R"(^(\d+)_)");
    for (const auto& v : o["result"].toObject()["cookies"].toArray()) {
        const QJsonObject c = v.toObject();
        if (!c["domain"].toString().endsWith("pixiv.net"))
            continue;
        pixivCookies << c;
        // ログイン中の PHPSESSID は "<ユーザーID>_<ランダム>" の形
        if (c["name"].toString() == "PHPSESSID") {
            const auto m = loggedIn.match(c["value"].toString());
            if (m.hasMatch())
                uid = m.captured(1);
        }
    }
    if (!uid.isEmpty()) {
        succeed(pixivCookies, uid);
    } else if (++polls_ >= 3) {
        // 起動直後は Cookie の読み込みが間に合わないことがあるので数回だけ見る
        send("Browser.close");
        finish(false, "pixiv へのログインが確認できませんでした。開いたブラウザで pixiv にログインしてから閉じてください");
    }
}

void ChromeLogin::succeed(const QJsonArray& cookies, const QString& uid)
{
    pollTimer_.stop();
    QStringList parts;
    for (const auto& v : cookies)
        parts << v.toObject()["name"].toString() + "=" + v.toObject()["value"].toString();

    pixiv::Session s;
    s.userId = uid;
    s.cookie = parts.join("; ");
    s.userAgent = userAgent_;
    s.savedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    try {
        s.save();
    } catch (const pixiv::Error& e) {
        send("Browser.close");
        finish(false, e.msg);
        return;
    }
    send("Browser.close");

    // 表示用にユーザー名を取る
    QNetworkRequest req(QUrl("https://www.pixiv.net/ajax/user/" + uid + "?lang=ja"));
    req.setRawHeader("Referer", "https://www.pixiv.net/");
    req.setRawHeader("Cookie", s.cookie.toUtf8());
    if (!userAgent_.isEmpty())
        req.setHeader(QNetworkRequest::UserAgentHeader, userAgent_);
    QNetworkReply* reply = nam_.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, s]() mutable {
        reply->deleteLater();
        s.userName = QJsonDocument::fromJson(reply->readAll()).object()["body"].toObject()["name"].toString();
        try {
            s.save();
        } catch (const pixiv::Error&) {
        }
        finish(true, "ログインしました");
    });
}

void ChromeLogin::finish(bool ok, const QString& message)
{
    if (phase_ == Idle)
        return;
    const bool reading = phase_ == Reading;
    phase_ = Idle;
    portTimer_.stop();
    pollTimer_.stop();
    QTimer::singleShot(500, this, [this] { ws_.close(); });
    // 画面なしで開いたブラウザは必ず閉じる
    if (reading && proc_) {
        QPointer<QProcess> p = proc_;
        QTimer::singleShot(3000, this, [p] {
            if (p && p->state() != QProcess::NotRunning)
                p->kill();
        });
    }
    emit finished(ok, message);
}
