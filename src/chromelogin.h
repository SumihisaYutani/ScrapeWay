#pragma once

#include "cdpsocket.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>

// PC の Chrome (無ければ Edge) を専用プロファイルで開いて pixiv にログインしてもらい、Cookie を保存する。
//  1. 普通のブラウザとして開く (デバッグ接続を付けると Google ログインが「安全でないブラウザ」として拒否されるため)
//  2. ユーザーがログインしてブラウザを閉じたら、同じプロファイルを画面なしで開き直して
//     DevTools の接続から Cookie を読む
class ChromeLogin : public QObject {
    Q_OBJECT
public:
    explicit ChromeLogin(QObject* parent = nullptr);
    void start();
    bool running() const { return phase_ != Idle; }

signals:
    void status(const QString& message);
    void finished(bool ok, const QString& message);

private:
    enum Phase { Idle, Interactive, Reading };

    static QString findBrowser();
    QProcess* launch(const QStringList& args);
    void startReading();
    void checkPort();
    void poll();
    void onMessage(const QString& text);
    void succeed(const QJsonArray& cookies, const QString& uid);
    void finish(bool ok, const QString& message);
    void send(const QString& method);

    Phase phase_ = Idle;
    QString exe_, profile_, userAgent_;
    QPointer<QProcess> proc_;
    CdpSocket ws_;
    QTimer portTimer_, pollTimer_;
    QElapsedTimer elapsed_;
    QNetworkAccessManager nam_;
    int nextId_ = 0, cookieReq_ = -1, polls_ = 0;
};
