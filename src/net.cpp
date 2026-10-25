#include "net.h"

#include <QDeadlineTimer>
#include <QEventLoop>
#include <QNetworkReply>
#include <QThread>

namespace net {
namespace {

const QString kDefaultUa =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36";

bool isPixivHost(const QString& host)
{
    return host.endsWith("pixiv.net") || host.endsWith("pximg.net");
}

}  // namespace

Http::Http(QString cookie, QString userAgent, const std::atomic<bool>* cancel)
    : cookie_(std::move(cookie)), userAgent_(std::move(userAgent)), cancel_(cancel)
{
}

Response Http::fetch(const QUrl& url, bool withCookie, int retries)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, userAgent_.isEmpty() ? kDefaultUa : userAgent_);
    req.setRawHeader("Accept-Language", "ja");
    // pixiv の画像サーバーは Referer が無いと 403 を返す
    if (isPixivHost(url.host()))
        req.setRawHeader("Referer", "https://www.pixiv.net/");
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    if (withCookie && !cookie_.isEmpty())
        req.setRawHeader("Cookie", cookie_.toUtf8());
    req.setTransferTimeout(60000);

    for (int attempt = 0;; ++attempt) {
        if (cancelled())
            throw Error("中止しました");
        QNetworkReply* reply = nam_.get(req);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        if (!reply->isFinished())
            loop.exec();
        Response r;
        r.code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r.body = reply->readAll();
        r.url = reply->url();
        r.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        r.error = reply->errorString();
        delete reply;

        const bool retryable = r.code == 429 || r.code >= 500 || r.code == 0;
        if (retryable && attempt < retries) {
            const double sec = r.code == 0 ? 5.0 * (attempt + 1) : 10.0 * (attempt + 1) * (attempt + 1);
            if (log_)
                log_(QString("  %1 のため %2 秒待ってやり直します（%3/%4）: %5")
                         .arg(r.code ? QString("HTTP %1").arg(r.code) : QString("通信エラー"))
                         .arg(sec)
                         .arg(attempt + 1)
                         .arg(retries)
                         .arg(url.toString()));
            wait(sec);
            continue;
        }
        return r;
    }
}

QByteArray Http::get(const QUrl& url, bool withCookie)
{
    const Response r = fetch(url, withCookie);
    if (r.code < 200 || r.code >= 300)
        throw Error(r.code ? QString("HTTP %1: %2").arg(r.code).arg(url.toString()) : r.error);
    return r.body;
}

void Http::wait(double sec)
{
    QDeadlineTimer deadline(qint64(sec * 1000));
    while (!deadline.hasExpired()) {
        if (cancelled())
            throw Error("中止しました");
        QThread::msleep(100);
    }
}

}  // namespace net
