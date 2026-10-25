#include "pixiv.h"

#include "paths.h"
#include "util.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace pixiv {
namespace {

QString str(const QJsonValue& v)
{
    if (v.isString())
        return v.toString();
    if (v.isDouble())
        return QString::number(qint64(v.toDouble()));
    return {};
}

QDateTime parseDate(const QString& s) { return QDateTime::fromString(s, Qt::ISODate); }

QString formatDate(const QString& s)
{
    const QDateTime d = parseDate(s);
    return d.isValid() ? d.toLocalTime().toString("yyyy/MM/dd HH:mm") : QString();
}

qint64 countChars(const QString& s)
{
    qint64 n = 0;
    for (char32_t c : s.toUcs4())
        if (!QChar::isSpace(c))
            ++n;
    return n;
}

QString fill(const QString& tmpl, const QHash<QString, QString>& values)
{
    static const QRegularExpression re(R"(\{\{([A-Z_]+)\}\})");
    return replaceEach(tmpl, re, [&](const QRegularExpressionMatch& m) { return values.value(m.captured(1)); });
}

QString downloadTo(Http& http, const QString& url, const QString& stemPath)
{
    const QByteArray data = http.get(QUrl(url), false);
    QString ext = QFileInfo(QUrl(url).path()).suffix();
    if (ext.isEmpty())
        ext = "jpg";
    const QString dest = stemPath + "." + ext;
    QDir().mkpath(QFileInfo(dest).path());
    writeFile(dest, data);
    return dest;
}

QString folderFor(const QJsonObject& n)
{
    QString base = pixivRoot() + "/" + str(n["userId"]) + "_" + safeName(n["userName"].toString(), 30);
    const QJsonObject nav = n["seriesNavData"].toObject();
    const QString leaf = str(n["id"]) + "_" + safeName(n["title"].toString());
    if (!nav.isEmpty()) {
        base += "/series_" + str(nav["seriesId"]) + "_" + safeName(nav["title"].toString());
        return base + "/" + QString("%1").arg(nav["order"].toInt(), 3, 10, QChar('0')) + "_" + leaf;
    }
    return base + "/" + leaf;
}

QHash<QString, LibNovel> savedIndex()
{
    QHash<QString, LibNovel> h;
    for (const auto& n : library())
        h.insert(n.id, n);
    return h;
}

Item itemFrom(const QJsonObject& w, const QHash<QString, LibNovel>& saved)
{
    Item it;
    it.id = str(w["id"]);
    it.title = w["title"].toString();
    it.seriesId = str(w["seriesId"]);
    it.seriesTitle = w["seriesTitle"].toString();
    it.order = w["seriesContentOrder"].toInt();
    it.textCount = qint64(w["textCount"].toDouble());
    it.createDate = w["createDate"].toString();
    it.updateDate = w["updateDate"].toString();
    it.r18 = w["xRestrict"].toInt() > 0;
    for (const auto& t : w["tags"].toArray())
        it.tags << t.toString();
    it.status = "new";
    if (saved.contains(it.id)) {
        const QDateTime now = parseDate(it.updateDate), old = parseDate(saved[it.id].uploadDate);
        it.status = (now.isValid() && old.isValid() && now > old) ? "updated" : "saved";
    }
    return it;
}

QList<QJsonObject> works(Http& http, const QString& uid, const QStringList& ids)
{
    QList<QJsonObject> out;
    for (qsizetype i = 0; i < ids.size(); i += 50) {
        QStringList q;
        for (const auto& id : ids.mid(i, 50))
            q << "ids[]=" + id;
        const QJsonObject body = http.api(QString("user/%1/profile/novels?%2&lang=ja").arg(uid, q.join('&'))).toObject();
        for (const auto& w : body["works"].toObject())
            out << w.toObject();
    }
    return out;
}

std::pair<QString, QString> parseUrl(const QString& raw)
{
    const QUrl u = QUrl::fromUserInput(raw.trimmed());
    if (!u.isValid() || !(u.host() == "pixiv.net" || u.host().endsWith(".pixiv.net")))
        throw Error("pixiv のユーザー・シリーズ・小説の URL を入力してください");
    const QString path = u.path();
    QRegularExpressionMatch m;
    if ((m = QRegularExpression(R"(/users/(\d+))").match(path)).hasMatch())
        return {"user", m.captured(1)};
    if ((m = QRegularExpression(R"(/novel/series/(\d+))").match(path)).hasMatch())
        return {"series", m.captured(1)};
    const QString id = QUrlQuery(u).queryItemValue("id");
    if (path.contains("/novel/show.php") && !id.isEmpty())
        return {"novel", id};
    if ((m = QRegularExpression(R"(^/n/(\d+))").match(path)).hasMatch())
        return {"novel", m.captured(1)};
    throw Error("pixiv のユーザー・シリーズ・小説の URL を入力してください");
}

QString bodyHtml(const QString& content, const QHash<QString, QString>& images)
{
    QString text = content.toHtmlEscaped();
    text = replaceEach(text, QRegularExpression(R"(\[\[rb:(.+?)&gt;(.+?)\]\])"), [](const auto& m) {
        return "<ruby>" + m.captured(1).trimmed() + "<rt>" + m.captured(2).trimmed() + "</rt></ruby>";
    });
    text = replaceEach(text, QRegularExpression(R"(\[\[jumpuri:(.+?)&gt;(.+?)\]\])"), [](const auto& m) {
        const QString label = m.captured(1).trimmed(), url = m.captured(2).trimmed();
        if (!url.startsWith("http"))
            return m.captured(0);
        return "<a href=\"" + url + "\" target=\"_blank\" rel=\"noopener\">" + label + "</a>";
    });
    text = replaceEach(text, QRegularExpression(R"(\[chapter:(.+?)\]\n?)"),
                       [](const auto& m) { return "<h2>" + m.captured(1) + "</h2>"; });
    text = replaceEach(text, QRegularExpression(R"(\[jump:(\d+)\])"), [](const auto& m) {
        return "<a href=\"#p" + m.captured(1) + "\">" + m.captured(1) + "ページへ</a>";
    });
    text = replaceEach(text, QRegularExpression(R"(\[((?:uploadedimage|pixivimage):[\d-]+)\]\n?)"), [&](const auto& m) {
        const QString src = images.value(m.captured(1));
        return src.isEmpty() ? "[" + m.captured(1) + "]"
                             : "<figure><img src=\"" + src.toHtmlEscaped() + "\" alt=\"\"></figure>";
    });
    const QStringList pages = text.split(QRegularExpression(R"(\n?\[newpage\]\n?)"));
    QStringList out;
    for (qsizetype i = 0; i < pages.size(); ++i)
        out << QString("<section class=\"page\" id=\"p%1\">").arg(i + 1) + pages[i] + "</section>";
    return out.join('\n');
}

QString navLink(const QJsonObject& nav, const char* key, const QString& label)
{
    const QJsonObject t = nav[key].toObject();
    if (t.isEmpty() || (t.contains("available") && !t["available"].toBool()))
        return "<span class=\"nav-dis\">" + label + "</span>";
    const QString folder = QString("%1_%2_%3")
                               .arg(t["order"].toInt(), 3, 10, QChar('0'))
                               .arg(str(t["id"]), safeName(t["title"].toString()));
    const QString href = "../" + QString::fromUtf8(QUrl::toPercentEncoding(folder)) + "/novel.html";
    return "<a href=\"" + href + "\" title=\"" + t["title"].toString().toHtmlEscaped() + "\">" + label + "</a>";
}

}  // namespace

// ---------- ログイン情報 ----------
Session Session::load()
{
    const QJsonObject o = QJsonDocument::fromJson(readFile(paths::sessionFile())).object();
    Session s;
    s.userId = o["user_id"].toString();
    s.userName = o["user_name"].toString();
    s.cookie = o["cookie"].toString();
    s.userAgent = o["user_agent"].toString();
    s.savedAt = o["saved_at"].toString();
    return s;
}

void Session::save() const
{
    const QJsonObject o{{"user_id", userId}, {"user_name", userName}, {"cookie", cookie},
                        {"user_agent", userAgent}, {"saved_at", savedAt}};
    QDir().mkpath(QFileInfo(paths::sessionFile()).path());
    writeFile(paths::sessionFile(), QJsonDocument(o).toJson());
}

void Session::clear() { QFile::remove(paths::sessionFile()); }

// ---------- 通信 ----------
QJsonValue Http::api(const QString& path)
{
    const net::Response r = fetch(QUrl("https://www.pixiv.net/ajax/" + path), true);
    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    if (r.code != 200 || o["error"].toBool()) {
        QString m = o["message"].toString();
        if (m.isEmpty())
            m = r.code ? QString("pixiv API エラー (HTTP %1)").arg(r.code) : r.error;
        throw Error(m);
    }
    return o["body"];
}

// ---------- URL → 作品一覧 ----------
QString pixivRoot() { return paths::libraryRoot() + "/pixiv"; }

bool isPixivUrl(const QString& url)
{
    const QString host = QUrl::fromUserInput(url.trimmed()).host();
    return host == "pixiv.net" || host.endsWith(".pixiv.net");
}

QString safeName(const QString& in, int limit)
{
    static const QRegularExpression bad(R"([\\/:*?"<>|\r\n\t])");
    auto trim = [](QString s) {
        while (!s.isEmpty() && (s.front() == ' ' || s.front() == '.'))
            s.remove(0, 1);
        while (!s.isEmpty() && (s.back() == ' ' || s.back() == '.'))
            s.chop(1);
        return s;
    };
    QString s = trim(QString(in).replace(bad, "_"));
    const QList<uint> ucs = s.toUcs4();
    if (ucs.size() > limit)
        s = trim(QString::fromUcs4(reinterpret_cast<const char32_t*>(ucs.constData()), limit));
    return s.isEmpty() ? "_" : s;
}

ResolveResult resolve(Http& http, const QString& url)
{
    const auto [kind, xid] = parseUrl(url);
    const auto saved = savedIndex();
    ResolveResult r;
    r.kind = kind;
    r.id = xid;
    QList<QJsonObject> ws;

    if (kind == "user") {
        const QJsonObject user = http.api(QString("user/%1?lang=ja").arg(xid)).toObject();
        const QJsonObject all = http.api(QString("user/%1/profile/all?lang=ja").arg(xid)).toObject();
        ws = works(http, xid, all["novels"].toObject().keys());
        r.title = user["name"].toString() + " さんの小説";
        if (ws.isEmpty())
            r.warning = http.loggedIn() ? "このユーザーの小説はありません"
                                        : "小説が見つかりません。R-18 作品の場合はログインが必要です";
    } else if (kind == "series") {
        const QJsonObject s = http.api(QString("novel/series/%1?lang=ja").arg(xid)).toObject();
        QStringList ids;
        for (const auto& t : http.api(QString("novel/series/%1/content_titles?lang=ja").arg(xid)).toArray()) {
            const QJsonObject o = t.toObject();
            if (!o.contains("available") || o["available"].toBool())
                ids << str(o["id"]);
        }
        ws = works(http, str(s["userId"]), ids);
        r.title = "シリーズ「" + s["title"].toString() + "」 / " + s["userName"].toString();
    } else {
        const QJsonObject n = http.api(QString("novel/%1?lang=ja").arg(xid)).toObject();
        const QJsonObject nav = n["seriesNavData"].toObject();
        QJsonArray tags;
        for (const auto& t : n["tags"].toObject()["tags"].toArray())
            tags << t.toObject()["tag"];
        ws << QJsonObject{{"id", n["id"]},
                          {"title", n["title"]},
                          {"seriesId", nav["seriesId"]},
                          {"seriesTitle", nav["title"]},
                          {"seriesContentOrder", nav["order"]},
                          {"textCount", n["characterCount"].toDouble(double(countChars(n["content"].toString())))},
                          {"createDate", n["createDate"]},
                          {"updateDate", n["uploadDate"]},
                          {"xRestrict", n["xRestrict"]},
                          {"tags", tags}};
        r.title = n["title"].toString() + " / " + n["userName"].toString();
    }

    QList<Item> inSeries, singles;
    for (const auto& w : ws) {
        Item it = itemFrom(w, saved);
        (it.seriesId.isEmpty() ? singles : inSeries) << it;
    }
    std::stable_sort(inSeries.begin(), inSeries.end(), [](const Item& a, const Item& b) {
        return a.seriesId != b.seriesId ? a.seriesId < b.seriesId : a.order < b.order;
    });
    std::stable_sort(singles.begin(), singles.end(),
                     [](const Item& a, const Item& b) { return a.createDate > b.createDate; });
    r.items = inSeries + singles;
    return r;
}

// ---------- 保存 ----------
Saved saveNovel(Http& http, const QString& nid, const QString& existingDir, const LogFn& log)
{
    QJsonObject n = http.api(QString("novel/%1?lang=ja").arg(nid)).toObject();
    const QString content = n.take("content").toString();
    const QString out = existingDir.isEmpty() ? folderFor(n) : existingDir;
    if (!QDir().mkpath(out))
        throw Error("フォルダを作成できません: " + QDir::toNativeSeparators(out));
    writeFile(out + "/content.txt", content.toUtf8());

    if (const QString cover = n["coverUrl"].toString(); !cover.isEmpty()) {
        try {
            for (const auto& old : QDir(out).entryList({"cover.*"}, QDir::Files))
                QFile::remove(out + "/" + old);
            downloadTo(http, cover, out + "/cover");
        } catch (const Error& e) {
            log("  表紙の取得に失敗: " + e.msg);
        }
    }

    // [uploadedimage:ID] 本文用にアップロードされた画像
    const QJsonObject embedded = n["textEmbeddedImages"].toObject();
    for (auto it = embedded.begin(); it != embedded.end(); ++it) {
        const QString url = it.value().toObject()["urls"].toObject()["original"].toString();
        if (url.isEmpty())
            continue;
        try {
            downloadTo(http, url, out + "/images/uploaded_" + it.key());
        } catch (const Error& e) {
            log("  挿絵の取得に失敗 " + it.key() + ": " + e.msg);
        }
    }
    // [pixivimage:作品ID] / [pixivimage:作品ID-ページ番号] イラスト作品の引用
    QStringList refs;
    for (const auto& m : QRegularExpression(R"(\[pixivimage:(\d+(?:-\d+)?)\])").globalMatch(content))
        if (!refs.contains(m.captured(1)))
            refs << m.captured(1);
    for (const auto& ref : refs) {
        const QString illust = ref.section('-', 0, 0);
        const int page = ref.contains('-') ? ref.section('-', 1).toInt() : 1;
        try {
            const QJsonArray pages = http.api(QString("illust/%1/pages?lang=ja").arg(illust)).toArray();
            if (page < 1 || page > pages.size())
                throw Error("ページがありません");
            downloadTo(http, pages[page - 1].toObject()["urls"].toObject()["original"].toString(),
                       out + "/images/pixiv_" + ref);
        } catch (const Error& e) {
            if (e.msg == "中止しました")
                throw;
            log("  挿絵の取得に失敗 " + ref + ": " + e.msg);
        }
    }

    n["_savedAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    // 文字数は pixiv の表示と揃える
    n["_textCount"] = n["characterCount"].toDouble(double(countChars(content)));
    // meta.json の存在 = 保存完了 とみなすので最後に書く
    writeFile(out + "/meta.json", QJsonDocument(n).toJson(QJsonDocument::Indented));
    renderHtml(out);
    return {out, n["title"].toString()};
}

// ---------- ライブラリ ----------
QList<LibNovel> library()
{
    QList<LibNovel> out;
    const QString root = pixivRoot();
    if (!QDir(root).exists())
        return out;
    QDirIterator it(root, {"meta.json"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString dir = QFileInfo(path).path();
        const QJsonObject m = QJsonDocument::fromJson(readFile(path)).object();
        if (m.isEmpty())
            continue;
        const QJsonObject nav = m["seriesNavData"].toObject();
        LibNovel n;
        n.id = str(m["id"]);
        n.title = m["title"].toString();
        n.userId = str(m["userId"]);
        n.userName = m["userName"].toString();
        n.seriesId = str(nav["seriesId"]);
        n.seriesTitle = nav["title"].toString();
        n.order = nav["order"].toInt();
        n.textCount = m.contains("characterCount") ? qint64(m["characterCount"].toDouble())
                    : m.contains("_textCount")     ? qint64(m["_textCount"].toDouble())
                                                   : countChars(QString::fromUtf8(readFile(dir + "/content.txt")));
        n.createDate = m["createDate"].toString();
        n.uploadDate = m["uploadDate"].toString();
        n.r18 = m["xRestrict"].toInt() > 0;
        for (const auto& t : m["tags"].toObject()["tags"].toArray())
            n.tags << t.toObject()["tag"].toString();
        n.dir = dir;
        const QStringList covers = QDir(dir).entryList({"cover.*"}, QDir::Files);
        if (!covers.isEmpty())
            n.cover = dir + "/" + covers.first();
        n.savedAt = m["_savedAt"].toString();
        out << n;
    }
    return out;
}

void rebuildAll()
{
    for (const auto& n : library()) {
        try {
            renderHtml(n.dir);
        } catch (const Error&) {
        }
    }
}

// ---------- pixiv 記法 → 閲覧用 HTML ----------
void renderHtml(const QString& dir)
{
    const QJsonObject n = QJsonDocument::fromJson(readFile(dir + "/meta.json")).object();
    const QString content = QString::fromUtf8(readFile(dir + "/content.txt"));
    QHash<QString, QString> images;
    for (const auto& f : QDir(dir + "/images").entryInfoList(QDir::Files)) {
        const QString stem = f.completeBaseName();
        if (stem.startsWith("uploaded_"))
            images.insert("uploadedimage:" + stem.mid(9), "images/" + f.fileName());
        else if (stem.startsWith("pixiv_"))
            images.insert("pixivimage:" + stem.mid(6), "images/" + f.fileName());
    }
    const QStringList covers = QDir(dir).entryList({"cover.*"}, QDir::Files);

    QString tags;
    for (const auto& t : n["tags"].toObject()["tags"].toArray())
        tags += "<span class=tag>" + t.toObject()["tag"].toString().toHtmlEscaped() + "</span>";
    const QString caption = htmlToText(n["description"].toString());
    const QJsonObject nav = n["seriesNavData"].toObject();
    QString series;
    if (!nav.isEmpty())
        series = navLink(nav, "prev", "← 前の話") + "<span>シリーズ「" + nav["title"].toString().toHtmlEscaped() + "」 第" +
                 QString::number(nav["order"].toInt()) + "話</span>" + navLink(nav, "next", "次の話 →");

    QFile tf(":/reader.html");
    if (!tf.open(QIODevice::ReadOnly))
        throw Error("閲覧用テンプレートを読めません");
    const QString html = fill(QString::fromUtf8(tf.readAll()), {
        {"TITLE", n["title"].toString().toHtmlEscaped()},
        {"COVER", covers.isEmpty() ? QString() : "<img class=\"cover\" src=\"" + covers.first().toHtmlEscaped() + "\" alt=\"\">"},
        {"AUTHOR", n["userName"].toString().toHtmlEscaped()},
        {"CREATED", formatDate(n["createDate"].toString())},
        {"UPDATED", formatDate(n["uploadDate"].toString())},
        {"CHARS", QString::number(qint64(n["characterCount"].toDouble(n["_textCount"].toDouble(double(countChars(content))))))},
        {"TAGS", tags},
        {"ID", str(n["id"]).toHtmlEscaped()},
        {"CAPTION", caption.trimmed().isEmpty() ? QString() : "<div class=\"caption\">" + caption.toHtmlEscaped() + "</div>"},
        {"SERIES_TOP", series.isEmpty() ? QString() : "<nav class=\"series top\">" + series + "</nav>"},
        {"SERIES", series.isEmpty() ? QString() : "<nav class=\"series\">" + series + "</nav>"},
        {"BODY", bodyHtml(content, images)},
    });
    writeFile(dir + "/novel.html", html.toUtf8());
}

}  // namespace pixiv
