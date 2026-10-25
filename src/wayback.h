#pragma once

// Wayback Machine (web.archive.org) に残っている古いサイトの取得・保存。
//
// 保存形式: <保存先>/wayback/<ホスト名>/
//   original/<パス>  Wayback から受け取ったファイルそのまま (文字コードも当時のまま)
//   view/<パス>      閲覧用。UTF-8 に変換し、保存済みのページ同士へリンクを張り替えたもの
//                    (original と site.json からいつでも作り直せる)
//   site.json        保存したページ・画像の一覧 (元の URL・撮影日時・文字コードなど)
//   index.html       保存したページの目次

#include "model.h"
#include "net.h"

namespace wayback {

using Error = net::Error;

// Internet Archive が一時停止中で、待っても戻らなかったときのエラー文 (保存ジョブはこれを受けたら打ち切る)
inline const QString kArchiveOffline = "Internet Archive（Wayback Machine）が一時停止中です。しばらくしてから保存し直してください";

bool isWaybackUrl(const QString& input);
QString root();
// 「ページが見つかりません」「閉鎖しました」などのお知らせページなら理由を返す (普通のページなら空)
QString goneReasonOf(const QString& html);

ResolveResult resolve(net::Http& http, const QString& input);

struct Saved {
    QString dir, file, title;
};
// url のページの「消えていない最新の版」を取り込む (新しい記録から順に見て、404 や閉鎖のお知らせなどは飛ばす)。
// 同じサイトの画像なども一緒に保存する
Saved savePage(net::Http& http, const QString& url, const LogFn& log);
void renderSite(const QString& siteDir);  // view/ と index.html を作り直す
void rebuildAll();

struct LibPage {
    QString url, title, ts, viewFile, savedAt;
    qint64 size = 0;
};
struct LibSite {
    QString host, dir, index;
    QList<LibPage> pages;
};
QList<LibSite> library();

}  // namespace wayback
