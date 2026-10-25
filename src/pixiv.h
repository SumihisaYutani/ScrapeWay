#pragma once

// pixiv 小説の取得・保存・ライブラリ管理。
//
// 保存形式: <保存先>/pixiv/<ユーザーID>_<名前>/[series_<ID>_<シリーズ名>/]<話数>_<ID>_<タイトル>/
//   content.txt  pixiv の本文そのまま ([newpage] [[rb:...]] などの記法を含む)
//   meta.json    API が返した作品情報 (本文以外) + "_" 始まりの保存情報
//   novel.html   閲覧用 HTML (content.txt と meta.json からいつでも作り直せる)
//   cover.*      表紙
//   images/      挿絵 (uploaded_<ID>.* / pixiv_<作品ID[-ページ]>.*)

#include "model.h"
#include "net.h"

#include <QJsonValue>

namespace pixiv {

using Error = net::Error;

struct Session {
    QString userId, userName, cookie, userAgent, savedAt;
    bool valid() const { return !cookie.isEmpty(); }
    static Session load();
    void save() const;
    static void clear();
};

// 保存したログイン情報を使う HTTP クライアント
class Http : public net::Http {
public:
    explicit Http(const Session& session = Session::load(), const std::atomic<bool>* cancel = nullptr)
        : net::Http(session.cookie, session.userAgent, cancel)
    {
    }
    QJsonValue api(const QString& path);  // https://www.pixiv.net/ajax/<path> の body
};

struct LibNovel {
    QString id, title, userId, userName, seriesId, seriesTitle;
    int order = 0;
    qint64 textCount = 0;
    QString createDate, uploadDate;
    bool r18 = false;
    QStringList tags;
    QString dir, cover, savedAt;
};

struct Saved {
    QString dir, title;
};

QString pixivRoot();
QString safeName(const QString& s, int limit = 40);
bool isPixivUrl(const QString& url);
ResolveResult resolve(Http& http, const QString& url);
Saved saveNovel(Http& http, const QString& id, const QString& existingDir, const LogFn& log);
QList<LibNovel> library();
void renderHtml(const QString& dir);
void rebuildAll();

}  // namespace pixiv
