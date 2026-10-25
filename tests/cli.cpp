// 開発用の確認ツール (配布物には含めない)
//   scrapeway_cli resolve <URL>        pixiv / Wayback の URL を一覧にする
//   scrapeway_cli save <URL> [件数]    未保存 (Wayback はおすすめの) ページを保存
//   scrapeway_cli library
#include "../src/paths.h"
#include "../src/pixiv.h"
#include "../src/textcodec.h"
#include "../src/wayback.h"

#include <QCoreApplication>
#include <QFile>
#include <QTextStream>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("ScrapeWay");
    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    const QStringList args = app.arguments().mid(1);
    if (args.isEmpty()) {
        out << "usage: scrapeway_cli resolve|save <URL> [limit] / library\n";
        return 2;
    }
    try {
        if (args[0] == "gone") {  // gone <HTMLファイル>... : 消えたページの判定を確かめる
            for (const auto& f : args.mid(1)) {
                QFile file(f);
                if (!file.open(QIODevice::ReadOnly))
                    continue;
                const QByteArray data = file.readAll();
                const QString reason = wayback::goneReasonOf(textcodec::decode(data, textcodec::detect(data)));
                out << (reason.isEmpty() ? "ok  " : "GONE") << "  " << f << "  " << reason << "\n";
            }
            return 0;
        }
        if (args[0] == "render") {
            pixiv::rebuildAll();
            wayback::rebuildAll();
            out << "rendered\n";
            return 0;
        }
        if (args[0] == "library") {
            for (const auto& n : pixiv::library())
                out << "pixiv\t" << n.id << "\t" << n.order << "\t" << n.title << "\t" << n.textCount << "\t" << n.dir << "\n";
            for (const auto& s : wayback::library())
                for (const auto& p : s.pages)
                    out << "wayback\t" << s.host << "\t" << p.ts << "\t" << p.title << "\t" << p.viewFile << "\n";
            return 0;
        }
        const QString url = args.value(1);
        const bool wb = wayback::isWaybackUrl(url) || !pixiv::isPixivUrl(url);
        pixiv::Http http(wb ? pixiv::Session{} : pixiv::Session::load());
        const auto r = wb ? wayback::resolve(http, url) : pixiv::resolve(http, url);
        out << r.kind << " " << r.id << " " << r.title << " warning=" << r.warning << " parent=" << r.parentUrl << "\n";
        for (const auto& i : r.items)
            out << "  [" << i.seriesId << "] " << i.id << " #" << i.order << " [" << i.status << (i.preselect ? "" : ",opt")
                << "] " << i.title << " " << i.textCount << " " << i.updateDate << (i.r18 ? " R18" : "") << "\n";
        out.flush();
        if (args[0] == "save") {
            const bool all = args.contains("--all");  // 保存済みも取り直す (動作確認用)
            const int limit = args.value(2, "1000").toInt();
            QHash<QString, QString> existing;
            for (const auto& n : pixiv::library())
                existing.insert(n.id, n.dir);
            QSet<QString> dirs;
            int n = 0;
            for (const auto& i : r.items) {
                if ((!all && i.status == "saved") || !i.preselect || n++ >= limit)
                    continue;
                const auto log = [&](const QString& m) { out << m << "\n"; };
                if (wb) {
                    const auto s = wayback::savePage(http, i.id, log);
                    dirs.insert(s.dir);
                    out << "saved: " << s.title << " -> " << s.file << "\n";
                } else {
                    const auto s = pixiv::saveNovel(http, i.id, existing.value(i.id), log);
                    out << "saved: " << s.title << " -> " << s.dir << "\n";
                }
                out.flush();
                http.wait(1);
            }
            for (const auto& d : dirs)
                wayback::renderSite(d);
        }
    } catch (const net::Error& e) {
        out << "error: " << e.msg << "\n";
        return 1;
    }
    return 0;
}
