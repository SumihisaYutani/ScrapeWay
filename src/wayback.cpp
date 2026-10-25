#include "wayback.h"

#include "paths.h"
#include "textcodec.h"
#include "util.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace wayback {
namespace {

const QString kCancelled = "中止しました";

struct Target {
    QString ts, url;
};

struct Link {
    QString url, label;
};

QString stripWww(const QString& host)
{
    const QString h = host.toLower();
    return h.startsWith("www.") ? h.mid(4) : h;
}

// 比較用に URL を整える (フラグメントを除き、既定のポート番号を省く)
QString normalizeUrl(QUrl u)
{
    u.setFragment({});
    u.setHost(u.host().toLower());
    if ((u.scheme() == "http" && u.port() == 80) || (u.scheme() == "https" && u.port() == 443))
        u.setPort(-1);
    if (u.path().isEmpty())
        u.setPath("/");
    return u.toString(QUrl::FullyEncoded);
}

// 同じページかどうかの判定に使うキー (http/https・www の有無は区別しない)
QString keyOf(const QString& url)
{
    const QUrl u(url);
    QString k = stripWww(u.host()) + (u.path(QUrl::FullyEncoded).isEmpty() ? "/" : u.path(QUrl::FullyEncoded));
    if (u.hasQuery())
        k += "?" + u.query(QUrl::FullyEncoded);
    return k;
}

bool sameSite(const QString& a, const QString& b) { return stripWww(QUrl(a).host()) == stripWww(QUrl(b).host()); }

QString rawUrl(const QString& ts, const QString& url) { return "https://web.archive.org/web/" + ts + "id_/" + url; }
QString viewUrl(const QString& ts, const QString& url) { return "https://web.archive.org/web/" + ts + "/" + url; }

QString tsFrom(const QUrl& waybackUrl, const QString& fallback)
{
    static const QRegularExpression re(R"(/web/(\d{14}))");
    const auto m = re.match(waybackUrl.path());
    return m.hasMatch() ? m.captured(1) : fallback;
}

QString tsToIso(const QString& ts)
{
    QDateTime d = QDateTime::fromString(ts.left(14), "yyyyMMddhhmmss");
    if (!d.isValid())
        d = QDateTime::fromString(ts.left(8), "yyyyMMdd");
    if (!d.isValid())
        return {};
    d.setTimeZone(QTimeZone::UTC);
    return d.toString(Qt::ISODate);
}

QString tsToText(const QString& ts)
{
    const QDateTime d = QDateTime::fromString(tsToIso(ts), Qt::ISODate);
    return d.isValid() ? d.toLocalTime().toString("yyyy/MM/dd") : ts;
}

Target parseTarget(const QString& input)
{
    const QString s = input.trimmed();
    static const QRegularExpression re(R"(^(?:https?://)?(?:web\.)?archive\.org/web/(\d{1,14})[a-z_]*\*?/(.+)$)",
                                       QRegularExpression::CaseInsensitiveOption);
    Target t;
    QString url;
    if (const auto m = re.match(s); m.hasMatch()) {
        t.ts = m.captured(1);
        url = m.captured(2);
    } else if (s.contains("archive.org/web/")) {
        throw Error("Wayback Machine の URL の形が読み取れません（例: https://web.archive.org/web/20050101000000/http://example.com/）");
    } else {
        t.ts = QDateTime::currentDateTimeUtc().toString("yyyyMMddhhmmss");
        url = s;
    }
    url.replace(QRegularExpression("^(https?):/+", QRegularExpression::CaseInsensitiveOption), "\\1://");
    if (!url.contains("://"))
        url = "http://" + url;
    const QUrl u(url);
    if (!u.isValid() || u.host().isEmpty())
        throw Error("URL が読み取れません: " + s);
    t.url = normalizeUrl(u);
    return t;
}

// ---------- ファイル名 ----------
QString cleanSegment(QString s)
{
    static const QRegularExpression bad(R"([\\/:*?"<>|#%\r\n\t])");
    static const QRegularExpression reserved(R"(^(con|prn|aux|nul|com\d|lpt\d)(\..*)?$)", QRegularExpression::CaseInsensitiveOption);
    s.replace(bad, "_");
    while (!s.isEmpty() && (s.back() == ' ' || s.back() == '.'))
        s.chop(1);
    while (!s.isEmpty() && s.front() == ' ')
        s.remove(0, 1);
    if (s.isEmpty() || s == "." || s == "..")
        s = "_";
    if (reserved.match(s).hasMatch())
        s.prepend('_');
    if (s.size() <= 80)
        return s;
    // 長すぎる名前は切り詰めるが、切った部分だけが違う別のページと重ならないよう、元の名前から作った印を付ける
    const QString mark = QString::fromLatin1(QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
    return s.left(71) + "~" + mark;
}

bool hasHtmlSuffix(const QString& name)
{
    const QString ext = QFileInfo(name).suffix().toLower();
    return ext == "html" || ext == "htm" || ext == "shtml" || ext == "xhtml";
}

// 元の URL → サイトフォルダ内の相対パス (例: novel/ch1.html, bbs.cgi@no=3.html)
QString localPath(const QString& url, bool html)
{
    const QUrl u(url);
    QString path = u.path(QUrl::FullyDecoded);
    if (path.isEmpty() || path.endsWith('/'))
        path += "index.html";
    QStringList segs;
    for (const auto& seg : path.split('/', Qt::SkipEmptyParts))
        segs << cleanSegment(seg);
    if (segs.isEmpty())
        segs << "index.html";
    const QString q = u.query(QUrl::FullyDecoded);
    if (!q.isEmpty())
        segs.last() += "@" + cleanSegment(q);
    if (html && (!hasHtmlSuffix(segs.last()) || !q.isEmpty()))
        segs.last() += ".html";
    return segs.join('/');
}

QString siteDirFor(const QString& url) { return root() + "/" + cleanSegment(QUrl(url).host().toLower()); }

QString relativeHref(const QString& fromDir, const QString& toFile)
{
    return QString::fromUtf8(QUrl::toPercentEncoding(QDir(fromDir).relativeFilePath(toFile), "/"));
}

// ---------- site.json ----------
QJsonObject loadManifest(const QString& dir) { return QJsonDocument::fromJson(readFile(dir + "/site.json")).object(); }

void saveManifest(const QString& dir, const QJsonObject& m)
{
    writeFile(dir + "/site.json", QJsonDocument(m).toJson(QJsonDocument::Indented));
}

// ---------- HTML の読み取り ----------
QString attrValue(const QRegularExpressionMatch& m, int first)
{
    for (int i = first; i < first + 3; ++i)
        if (m.capturedStart(i) >= 0)
            return m.captured(i);
    return {};
}

// href などの値を絶対 URL にする。ページ内リンクや javascript: などは空を返す
QString resolveUrl(const QString& base, QString value)
{
    value = value.trimmed().replace("&amp;", "&");
    if (value.isEmpty() || value.startsWith('#'))
        return {};
    const QUrl r = QUrl(base).resolved(QUrl(value));
    if (r.scheme() != "http" && r.scheme() != "https")
        return {};
    return normalizeUrl(r);
}

QString titleOf(const QString& html)
{
    static const QRegularExpression re(R"(<title[^>]*>(.*?)</title>)",
                                       QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    const auto m = re.match(html);
    return m.hasMatch() ? htmlToText(m.captured(1)).simplified() : QString();
}

QList<Link> extractLinks(const QString& html, const QString& base)
{
    static const QRegularExpression aRe(
        R"re(<a\b[^>]*?\shref\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))[^>]*>(.*?)</a\s*>)re",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression frameRe(R"re(<(?:frame|iframe|area)\b[^>]*?\s(?:src|href)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))re",
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression altRe(R"(\salt\s*=\s*["']([^"']*)["'])", QRegularExpression::CaseInsensitiveOption);
    QList<Link> out;
    for (const auto& m : aRe.globalMatch(html)) {
        const QString url = resolveUrl(base, attrValue(m, 1));
        if (url.isEmpty())
            continue;
        QString label = htmlToText(m.captured(4)).simplified();
        if (label.isEmpty())
            label = htmlToText(altRe.match(m.captured(4)).captured(1)).simplified();
        if (label.isEmpty()) {
            // 「アイコン画像がリンクで、題名はその後ろの文字」という作りが多いので、後ろの文字を使う
            static const QRegularExpression stop(R"(<(br|a\s|/td|/li|p[\s>]|tr|/tr|hr|div|/div|img))",
                                                 QRegularExpression::CaseInsensitiveOption);
            const QString after = html.mid(m.capturedEnd(), 400);
            const auto s = stop.match(after);
            label = htmlToText(s.hasMatch() ? after.left(s.capturedStart()) : after).simplified().left(60);
        }
        out << Link{url, label};
    }
    for (const auto& m : frameRe.globalMatch(html))
        if (const QString url = resolveUrl(base, attrValue(m, 1)); !url.isEmpty())
            out << Link{url, {}};
    return out;
}

// ページと一緒に保存する画像・CSS・BGM など
QStringList extractAssets(const QString& html, const QString& base)
{
    static const QRegularExpression re(
        R"re(<(img|input|body|table|td|th|embed|bgsound|script|link)\b([^>]*?)\s(src|background|href)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))re",
        QRegularExpression::CaseInsensitiveOption);
    QStringList out;
    for (const auto& m : re.globalMatch(html)) {
        const QString tag = m.captured(1).toLower(), attr = m.captured(3).toLower();
        if (attr == "href" && tag != "link")
            continue;
        if (attr == "background" && tag != "body" && tag != "table" && tag != "td" && tag != "th")
            continue;
        const QString url = resolveUrl(base, attrValue(m, 4));
        if (url.isEmpty())
            continue;
        // <link> はスタイルシートとアイコンだけ (rel="next" などのページへのリンクは除く)
        if (tag == "link" && !QRegularExpression(R"(\.(css|ico|png|gif|jpe?g)$)", QRegularExpression::CaseInsensitiveOption)
                                  .match(QUrl(url).path())
                                  .hasMatch())
            continue;
        if (!out.contains(url))
            out << url;
    }
    return out;
}

bool looksLikePage(const QString& url)
{
    static const QRegularExpression asset(R"(\.(jpe?g|png|gif|bmp|ico|css|js|mid|midi|mp3|wav|swf|zip|lzh|exe|pdf)$)",
                                          QRegularExpression::CaseInsensitiveOption);
    return !asset.match(QUrl(url).path()).hasMatch();
}

// ---------- Wayback との通信 ----------
// Internet Archive は混雑すると「Temporarily Offline」のお知らせページを返すことがある。
// それを本物のページや「記録なし」と取り違えないよう、Wayback への通信はすべてここを通す
net::Response fetchArchive(net::Http& http, const QUrl& url, int retries = 3)
{
    for (int i = 0;; ++i) {
        net::Response r = http.fetch(url, false, retries);
        const bool offline = r.body.contains("<title>Internet Archive: Temporar") || r.body.contains("Temporarily Offline</h1>");
        if (!offline)
            return r;
        if (i >= 3)
            throw Error(kArchiveOffline);
        http.notify("  Internet Archive が一時停止中のため、60 秒待ってやり直します");
        http.wait(60);
    }
}

// CDX API の結果 (JSON の配列) を返す。結果が空なら空の配列、形が違えばエラー
QJsonArray cdxRows(net::Http& http, const QUrl& cdx)
{
    const net::Response r = fetchArchive(http, cdx);
    if (r.code != 200)
        throw Error(r.code ? QString("Wayback Machine の一覧を取得できませんでした (HTTP %1)").arg(r.code) : r.error);
    if (r.body.trimmed().isEmpty())
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(r.body);
    if (!doc.isArray())
        throw Error("Wayback Machine の一覧を取得できませんでした（混雑しているか、一時停止中の可能性があります）");
    return doc.array();
}

// ---------- 保存する版の選択 ----------
struct Capture {
    QString ts, digest;
};

// そのページの正常 (200) な記録を新しい順に返す。内容が同じ記録は一番新しい1つにまとめる
QList<Capture> goodCaptures(net::Http& http, const QString& url)
{
    QUrl cdx("https://web.archive.org/cdx/search/cdx");
    cdx.setQuery("url=" + QString::fromLatin1(QUrl::toPercentEncoding(url)) +
                 "&output=json&fl=timestamp,digest&filter=statuscode:200");
    const QJsonArray rows = cdxRows(http, cdx);
    QList<Capture> out;
    QSet<QString> seen;
    for (qsizetype i = rows.size() - 1; i >= 1; --i) {  // 1 行目は列名。古い順に並んでいるので後ろから
        const QJsonArray row = rows[i].toArray();
        if (row.size() < 2 || seen.contains(row[1].toString()))
            continue;
        seen.insert(row[1].toString());
        out << Capture{row[0].toString(), row[1].toString()};
    }
    return out;
}

// Wayback の URL (…/web/<時刻>id_/<元の URL>) から元の URL を取り出す
QString originalFrom(const QUrl& waybackUrl)
{
    static const QRegularExpression re(R"(/web/\d+[a-z_]*/(.+)$)");
    const auto m = re.match(waybackUrl.toString(QUrl::FullyEncoded));
    if (!m.hasMatch())
        return {};
    QString url = m.captured(1);
    url.replace(QRegularExpression("^(https?):/+", QRegularExpression::CaseInsensitiveOption), "\\1://");
    return normalizeUrl(QUrl(url));
}

bool isHtml(const net::Response& r)
{
    return r.contentType.contains("html") || r.body.left(2048).toLower().contains("<html");
}

// 正常 (200) として記録されていても、中身が「ページが見つかりません」「閉鎖しました」などのお知らせなら理由を返す
QString goneReason(QString html)
{
    static const QRegularExpression titleRe(
        R"(404|not\s*found|forbidden|\berror\b|エラー|見つかりません|存在しません|削除されました|閉鎖|移転しました|サービス.{0,4}終了|ジオシティーズ)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression bodyRe(
        R"(not\s*found|ページ(が|は)見つかりません|お探しのページ|ファイルが見つかりません|存在しません|削除されました|閉鎖しました|閉鎖いたしました|移転しました|サービスを終了|公開を終了)",
        QRegularExpression::CaseInsensitiveOption);
    const QString title = titleOf(html);
    if (titleRe.match(title).hasMatch())
        return "題名が「" + title + "」";
    // 本文の言葉で判断するのは、短いページだけ (小説の本文にたまたま出てくる言葉で誤判定しないように)
    html.remove(QRegularExpression(R"(<(script|style)\b.*?</\1\s*>)",
                                   QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption));
    const QString text = htmlToText(html).simplified();
    if (text.size() < 1500)
        if (const auto m = bodyRe.match(text); m.hasMatch())
            return "「" + m.captured(0) + "」と書かれている";
    return {};
}

// ---------- 閲覧用 HTML ----------
QString rewriteHtml(const QString& html, const QString& pageUrl, const QString& pageTs, const QString& siteDir,
                    const QString& pageFile, const QHash<QString, QString>& pages, const QHash<QString, QString>& assets,
                    const QString& title)
{
    const QString fromDir = QFileInfo(siteDir + "/view/" + pageFile).path();
    static const QRegularExpression attrRe(
        R"re((<[a-zA-Z][^>]*?\s)(href|src|background|action)(\s*=\s*)(?:"([^"]*)"|'([^']*)'|([^\s>]+)))re",
        QRegularExpression::CaseInsensitiveOption);
    QString out = replaceEach(html, attrRe, [&](const QRegularExpressionMatch& m) -> QString {
        const QString value = attrValue(m, 4);
        const QString url = resolveUrl(pageUrl, value);
        if (url.isEmpty())
            return m.captured(0);
        const QString k = keyOf(url);
        QString target;
        if (pages.contains(k))
            target = relativeHref(fromDir, siteDir + "/view/" + pages[k]);
        else if (assets.contains(k))
            target = relativeHref(fromDir, siteDir + "/original/" + assets[k]);
        else
            target = viewUrl(pageTs, url);  // 保存していないものは Wayback 上のページへ
        const QString frag = QUrl(value.trimmed().replace("&amp;", "&")).fragment(QUrl::FullyEncoded);
        if (!frag.isEmpty())
            target += "#" + frag;
        return m.captured(1) + m.captured(2) + m.captured(3) + "\"" + target.toHtmlEscaped() + "\"";
    });

    // 文字コードの指定を UTF-8 に差し替える
    static const QRegularExpression metaCharset(R"(<meta[^>]*charset[^>]*>)", QRegularExpression::CaseInsensitiveOption);
    out.remove(metaCharset);
    static const QRegularExpression headRe(R"(<head\b[^>]*>)", QRegularExpression::CaseInsensitiveOption);
    const auto hm = headRe.match(out);
    const QString meta = "<meta charset=\"utf-8\">";
    if (hm.hasMatch())
        out.insert(hm.capturedEnd(), meta);
    else
        out.prepend(meta);

    // 先頭に「どこから保存したか」の帯を入れる (フレーム分割のページには入れない)
    static const QRegularExpression bodyRe(R"(<body\b[^>]*>)", QRegularExpression::CaseInsensitiveOption);
    if (!out.contains(QRegularExpression("<frameset", QRegularExpression::CaseInsensitiveOption))) {
        const QString index = relativeHref(fromDir, siteDir + "/index.html");
        const QString bar =
            "<div style=\"all:initial;display:block;font:12px/1.6 sans-serif;background:#fff8dc;color:#333;"
            "border-bottom:1px solid #e0d6a8;padding:3px 10px\">Wayback Machine から保存（" + tsToText(pageTs) +
            " 時点）　<a style=\"color:#2a5db0\" href=\"" + index + "\">保存したページの一覧</a>　<a style=\"color:#2a5db0\" href=\"" +
            viewUrl(pageTs, pageUrl).toHtmlEscaped() + "\" target=\"_blank\">Wayback で開く</a>" +
            (title.isEmpty() ? QString() : "　" + title.toHtmlEscaped()) + "</div>";
        const auto bm = bodyRe.match(out);
        if (bm.hasMatch())
            out.insert(bm.capturedEnd(), bar);
    }
    return out;
}

void writeIndex(const QString& dir, const QJsonObject& man)
{
    struct Row {
        QString file, title, url, ts;
    };
    QList<Row> rows;
    const QJsonObject pages = man["pages"].toObject();
    for (const auto& v : pages) {
        const QJsonObject p = v.toObject();
        rows << Row{p["file"].toString(), p["title"].toString(), p["url"].toString(), p["ts"].toString()};
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.file < b.file; });
    QString list;
    for (const auto& r : rows)
        list += "<tr><td><a href=\"view/" + QString::fromUtf8(QUrl::toPercentEncoding(r.file, "/")) + "\">" +
                (r.title.isEmpty() ? r.file : r.title).toHtmlEscaped() + "</a></td><td>" + tsToText(r.ts) +
                "</td><td class=u>" + r.url.toHtmlEscaped() + "</td></tr>\n";
    const QString host = man["host"].toString().toHtmlEscaped();
    const QString html =
        "<!doctype html><html lang=\"ja\"><head><meta charset=\"utf-8\"><title>" + host +
        "（保存したページ）</title><style>body{font:14px/1.6 system-ui,sans-serif;margin:24px;color:#222}"
        "table{border-collapse:collapse}td{padding:4px 12px 4px 0;border-bottom:1px solid #eee;vertical-align:top}"
        ".u{color:#888;font-size:12px;word-break:break-all}</style></head><body><h1>" + host + "</h1><p>Wayback Machine から保存した " +
        QString::number(rows.size()) + " ページ</p><table>\n" + list + "</table></body></html>\n";
    writeFile(dir + "/index.html", html.toUtf8());
}

}  // namespace

// ---------- 公開関数 ----------
bool isWaybackUrl(const QString& input)
{
    return input.contains(QRegularExpression(R"(^(https?://)?(web\.)?archive\.org/web/)", QRegularExpression::CaseInsensitiveOption));
}

QString root() { return paths::libraryRoot() + "/wayback"; }

QString goneReasonOf(const QString& html) { return goneReason(html); }

ResolveResult resolve(net::Http& http, const QString& input)
{
    const Target t = parseTarget(input);
    const QUrl u(t.url);
    QString dirPath = u.path();
    dirPath = dirPath.left(dirPath.lastIndexOf('/') + 1);
    if (dirPath.isEmpty())
        dirPath = "/";

    ResolveResult r;
    r.kind = "wayback";
    r.id = t.ts;

    // 開いたページ自体を読んで、そこからのリンク (目次 → 各話など) を拾う。
    // 保存するのは「消えていない最新の版」なので、その版と、指定された日時の版の両方からリンクを集める
    QString startTitle;
    QList<Link> links;
    auto addLinksFrom = [&](const net::Response& resp) {
        if (resp.code != 200 || keyOf(originalFrom(resp.url)) != keyOf(t.url))
            return false;
        const QString html = textcodec::decode(resp.body, textcodec::detect(resp.body, resp.contentType));
        if (isHtml(resp) && !goneReason(html).isEmpty())
            return false;
        if (startTitle.isEmpty())
            startTitle = titleOf(html);
        for (const auto& l : extractLinks(html, t.url))
            if (sameSite(l.url, t.url) && looksLikePage(l.url))
                links << l;
        return true;
    };
    bool startOk = false;
    try {
        int tried = 0;
        for (const auto& c : goodCaptures(http, t.url)) {
            if (++tried > 3)
                break;
            if (addLinksFrom(fetchArchive(http, QUrl(rawUrl(c.ts, t.url))))) {
                startOk = true;
                break;
            }
        }
    } catch (const Error& e) {
        if (e.msg == kCancelled || e.msg == kArchiveOffline)
            throw;
    }
    const net::Response atTs = fetchArchive(http, QUrl(rawUrl(t.ts, t.url)));
    startOk = addLinksFrom(atTs) || startOk;
    if (!startOk)
        r.warning = "このページは Wayback Machine に正常な記録がありません。同じフォルダ内の保存ページを表示します";

    // 同じフォルダ以下に保存されているページの一覧 (CDX API)
    QUrlQuery q;
    q.addQueryItem("url", stripWww(u.host()) + dirPath);
    q.addQueryItem("matchType", "prefix");
    q.addQueryItem("output", "json");
    q.addQueryItem("fl", "original,timestamp,mimetype,length");
    q.addQueryItem("filter", "statuscode:200");
    q.addQueryItem("collapse", "urlkey");
    q.addQueryItem("limit", "10000");
    QUrl cdx("https://web.archive.org/cdx/search/cdx");
    cdx.setQuery(q);
    const QJsonArray rows = cdxRows(http, cdx);

    const QJsonObject savedPages = loadManifest(siteDirFor(t.url))["pages"].toObject();
    auto status = [&](const QString& url) { return savedPages.contains(keyOf(url)) ? "saved" : "new"; };
    auto relTitle = [&](const QString& url) {
        const QUrl x(url);
        QString p = x.path(QUrl::FullyDecoded);
        if (p.startsWith(dirPath))
            p = p.mid(dirPath.size());
        if (x.hasQuery())
            p += "?" + x.query(QUrl::FullyDecoded);
        return p.isEmpty() ? x.path(QUrl::FullyDecoded) : p;
    };

    QList<Item> linked, others;
    QSet<QString> seen{keyOf(t.url)}, inCdx;
    Item first;
    first.id = t.url;
    first.title = startTitle.isEmpty() ? relTitle(t.url) : startTitle;
    first.seriesId = "0";
    first.seriesTitle = "開いたページ";
    first.updateDate = tsToIso(t.ts);
    first.status = status(t.url);

    QHash<QString, int> linkIndex;
    for (qsizetype i = 0; i < links.size(); ++i) {
        const QString k = keyOf(links[i].url);
        if (seen.contains(k) || linkIndex.contains(k))
            continue;
        linkIndex.insert(k, int(linked.size()));
        Item it;
        it.id = links[i].url;
        it.title = links[i].label.isEmpty() ? relTitle(links[i].url) : links[i].label;
        it.tags << relTitle(links[i].url);
        it.seriesId = "1";
        it.seriesTitle = "このページからのリンク";
        it.order = int(linked.size()) + 1;
        it.status = status(links[i].url);
        linked << it;
    }
    for (qsizetype i = 1; i < rows.size(); ++i) {  // 1 行目は列名
        const QJsonArray row = rows[i].toArray();
        if (row.size() < 4)
            continue;
        const QString mime = row[2].toString();
        if (!mime.contains("html") && mime != "text/plain")
            continue;
        const QString url = normalizeUrl(QUrl(row[0].toString()));
        const QString k = keyOf(url);
        if (k == keyOf(t.url)) {
            first.textCount = row[3].toString().toLongLong();
            continue;
        }
        if (linkIndex.contains(k)) {
            Item& it = linked[linkIndex[k]];
            it.textCount = row[3].toString().toLongLong();
            it.updateDate = tsToIso(row[1].toString());
            inCdx.insert(k);
            continue;
        }
        if (seen.contains(k))
            continue;
        seen.insert(k);
        Item it;
        it.id = url;
        it.title = relTitle(url);
        it.seriesId = "2";
        it.seriesTitle = "同じフォルダ内のその他の保存ページ";
        it.textCount = row[3].toString().toLongLong();
        it.updateDate = tsToIso(row[1].toString());
        it.status = status(url);
        it.preselect = false;
        others << it;
    }
    std::sort(others.begin(), others.end(), [](const Item& a, const Item& b) { return a.title < b.title; });

    // 同じフォルダ内なのに一覧に無いリンク先は、Wayback に一度も保存されていない
    const bool complete = rows.size() <= 10000;
    for (auto& it : linked) {
        const QUrl x(it.id);
        if (complete && it.status != "saved" && x.path().startsWith(dirPath) && !inCdx.contains(keyOf(it.id))) {
            it.status = "missing";
            it.preselect = false;
        }
    }

    r.items = QList<Item>{first} + linked + others;
    r.title = "Wayback: " + (startTitle.isEmpty() ? u.host() + u.path() : startTitle);
    if (dirPath != "/") {
        QString parent = dirPath.left(dirPath.size() - 1);
        parent = parent.left(parent.lastIndexOf('/') + 1);
        QUrl p = u;
        p.setPath(parent);
        p.setQuery(QString());
        r.parentUrl = viewUrl(t.ts, normalizeUrl(p));
    }
    if (rows.size() <= 1 && !startOk)
        r.warning = "このフォルダには Wayback Machine に保存されたページが見つかりません";
    return r;
}

Saved savePage(net::Http& http, const QString& url, const LogFn& log)
{
    const QString dir = siteDirFor(url);
    QJsonObject man = loadManifest(dir);
    QJsonObject pages = man["pages"].toObject();
    QJsonObject assets = man["assets"].toObject();
    QSet<QString> gone;  // 「消えたページ」と判断した内容 (サイト内で共通の閉鎖のお知らせなどを何度も取り直さないため)
    for (const auto& v : man["goneDigests"].toArray())
        gone.insert(v.toString());
    const QString k = keyOf(url);
    const QJsonObject prev = pages[k].toObject();
    auto saveGone = [&] {
        QJsonArray arr;
        for (const auto& d : std::as_const(gone))
            arr << d;
        man["goneDigests"] = arr;
    };

    // 新しい記録から順に見て、消えていない最初の版を使う
    const QList<Capture> caps = goodCaptures(http, url);
    if (caps.isEmpty())
        throw Error("Wayback Machine に正常な記録がありません");
    net::Response r;
    QString ts, digest;
    int tried = 0;
    for (const auto& c : caps) {
        if (gone.contains(c.digest))
            continue;
        if (!prev.isEmpty() && prev["ts"].toString() == c.ts && QFileInfo::exists(dir + "/original/" + prev["file"].toString())) {
            log("  最新の版（" + tsToText(c.ts) + "）は保存済みのため、取得し直しません");
            return {dir, prev["file"].toString(), prev["title"].toString().isEmpty() ? prev["file"].toString() : prev["title"].toString()};
        }
        if (++tried > 6)
            break;
        if (tried > 1)
            http.wait(1);
        const net::Response cand = fetchArchive(http, QUrl(rawUrl(c.ts, url)));
        if (cand.code != 200) {
            log(QString("  %1 の版を取得できません（%2）").arg(tsToText(c.ts), cand.code ? QString("HTTP %1").arg(cand.code) : cand.error));
            continue;
        }
        if (keyOf(originalFrom(cand.url)) != k) {
            log("  " + tsToText(c.ts) + " の版は別のページへ転送されるため使いません");
            continue;
        }
        if (isHtml(cand)) {
            const QString reason = goneReason(textcodec::decode(cand.body, textcodec::detect(cand.body, cand.contentType)));
            if (!reason.isEmpty()) {
                log("  " + tsToText(c.ts) + " の版は消えたページのようです（" + reason + "）。それより前の版を探します");
                gone.insert(c.digest);
                continue;
            }
        }
        r = cand;
        ts = tsFrom(cand.url, c.ts);
        digest = c.digest;
        break;
    }
    if (ts.isEmpty()) {
        saveGone();
        saveManifest(dir, man);
        throw Error("消えていない版が見つかりませんでした");
    }
    if (ts != caps.first().ts)
        log("  最新の記録は使えないため、" + tsToText(ts) + " の版を保存します");

    const bool html = isHtml(r);
    const bool text = r.contentType.startsWith("text/plain");
    const QString file = prev.isEmpty() ? localPath(url, html) : prev["file"].toString();
    QDir().mkpath(QFileInfo(dir + "/original/" + file).path());
    writeFile(dir + "/original/" + file, r.body);

    QString charset, title;
    if (html || text)
        charset = textcodec::detect(r.body, r.contentType);
    if (html) {
        const QString decoded = textcodec::decode(r.body, charset);
        title = titleOf(decoded);
        for (const QString& a : extractAssets(decoded, url)) {
            const QString ak = keyOf(a);
            if (!sameSite(a, url) || assets.contains(ak) || pages.contains(ak))
                continue;
            try {
                http.wait(0.5);
                // 付属ファイルはページ本体ほど大事ではないので、やり直しは1回だけ
                const net::Response ar = fetchArchive(http, QUrl(rawUrl(ts, a)), 1);
                if (ar.code != 200) {
                    log(QString("  画像などの取得に失敗 (%1): %2").arg(ar.code ? QString("HTTP %1").arg(ar.code) : ar.error, a));
                    // Wayback 側で壊れている記録 (502 など) も多いので、取れなかったものは次のページで聞き直さない
                    assets[ak] = QJsonObject{{"url", a}, {"missing", true}, {"status", ar.code}};
                    continue;
                }
                const QString afile = localPath(a, false);
                QDir().mkpath(QFileInfo(dir + "/original/" + afile).path());
                writeFile(dir + "/original/" + afile, ar.body);
                assets[ak] = QJsonObject{{"url", a}, {"file", afile}, {"ts", tsFrom(ar.url, ts)},
                                         {"contentType", ar.contentType}};
            } catch (const Error& e) {
                if (e.msg == kCancelled || e.msg == kArchiveOffline)
                    throw;
                log("  画像などの取得に失敗: " + a + ": " + e.msg);
            }
        }
    }

    pages[k] = QJsonObject{{"url", url},
                           {"file", file},
                           {"ts", ts},
                           {"digest", digest},
                           {"contentType", r.contentType},
                           {"charset", charset},
                           {"html", html},
                           {"title", title},
                           {"size", double(r.body.size())},
                           {"savedAt", QDateTime::currentDateTime().toString(Qt::ISODate)}};
    man["host"] = QUrl(url).host();
    man["pages"] = pages;
    man["assets"] = assets;
    saveGone();
    saveManifest(dir, man);
    return {dir, file, (title.isEmpty() ? file : title) + "（" + tsToText(ts) + " の版）"};
}

void renderSite(const QString& dir)
{
    const QJsonObject man = loadManifest(dir);
    const QJsonObject pages = man["pages"].toObject(), assets = man["assets"].toObject();
    QHash<QString, QString> pageFiles, assetFiles;
    for (auto it = pages.begin(); it != pages.end(); ++it)
        pageFiles.insert(it.key(), it.value().toObject()["file"].toString());
    for (auto it = assets.begin(); it != assets.end(); ++it)
        if (const QString f = it.value().toObject()["file"].toString(); !f.isEmpty())
            assetFiles.insert(it.key(), f);

    for (const auto& v : pages) {
        const QJsonObject p = v.toObject();
        const QString file = p["file"].toString();
        const QByteArray raw = readFile(dir + "/original/" + file);
        const QString viewPath = dir + "/view/" + file;
        QDir().mkpath(QFileInfo(viewPath).path());
        const QString charset = p["charset"].toString();
        if (p["html"].toBool()) {
            const QString html = textcodec::decode(raw, charset.isEmpty() ? textcodec::detect(raw) : charset);
            writeFile(viewPath, rewriteHtml(html, p["url"].toString(), p["ts"].toString(), dir, file, pageFiles,
                                            assetFiles, p["title"].toString())
                                    .toUtf8());
        } else if (!charset.isEmpty()) {
            // テキストのページは UTF-8 (BOM 付き) にしてブラウザで文字化けしないようにする
            writeFile(viewPath, "\xEF\xBB\xBF" + textcodec::decode(raw, charset).toUtf8());
        } else {
            writeFile(viewPath, raw);
        }
    }
    writeIndex(dir, man);
}

void rebuildAll()
{
    for (const auto& site : library()) {
        try {
            renderSite(site.dir);
        } catch (const Error&) {
        }
    }
}

QList<LibSite> library()
{
    QList<LibSite> out;
    for (const auto& info : QDir(root()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString dir = info.absoluteFilePath();
        const QJsonObject man = loadManifest(dir);
        if (man.isEmpty())
            continue;
        LibSite s;
        s.host = man["host"].toString();
        s.dir = dir;
        s.index = dir + "/index.html";
        for (const auto& v : man["pages"].toObject()) {
            const QJsonObject p = v.toObject();
            s.pages << LibPage{p["url"].toString(), p["title"].toString(), p["ts"].toString(),
                               dir + "/view/" + p["file"].toString(), p["savedAt"].toString(), qint64(p["size"].toDouble())};
        }
        std::sort(s.pages.begin(), s.pages.end(), [](const LibPage& a, const LibPage& b) { return a.viewFile < b.viewFile; });
        out << s;
    }
    return out;
}

}  // namespace wayback
