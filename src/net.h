#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QString>
#include <QUrl>

#include <atomic>
#include <functional>
#include <stdexcept>

namespace net {

class Error : public std::runtime_error {
public:
    explicit Error(const QString& m) : std::runtime_error(m.toStdString()), msg(m) {}
    QString msg;
};

struct Response {
    int code = 0;
    QByteArray body;
    QUrl url;  // リダイレクト後の URL
    QString contentType;
    QString error;
};

// 同期的に使う HTTP クライアント。作ったスレッドの中だけで使う。
// 429 / 5xx / 通信エラーは待ってから最大 3 回やり直す
class Http {
public:
    explicit Http(QString cookie = {}, QString userAgent = {}, const std::atomic<bool>* cancel = nullptr);
    // retries: 429 / 5xx / 通信エラーのときにやり直す回数
    Response fetch(const QUrl& url, bool withCookie = false, int retries = 3);
    void setLog(std::function<void(const QString&)> log) { log_ = std::move(log); }  // やり直しで待つことを知らせる先
    void notify(const QString& message) const
    {
        if (log_)
            log_(message);
    }
    QByteArray get(const QUrl& url, bool withCookie = false);  // 2xx 以外は Error
    void wait(double sec);                                     // 中止されたら Error を投げる
    bool loggedIn() const { return !cookie_.isEmpty(); }

private:
    bool cancelled() const { return cancel_ && cancel_->load(); }

    QNetworkAccessManager nam_;
    QString cookie_, userAgent_;
    const std::atomic<bool>* cancel_;
    std::function<void(const QString&)> log_;
};

}  // namespace net
