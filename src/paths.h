#pragma once

#include <QString>

// 設定と保存場所。exe と同じ場所に portable.txt があれば、設定もログイン情報も exe の隣の data/ に置く
namespace paths {

QString dataDir();
QString profileDir();   // ログイン用ブラウザの専用プロファイル
QString sessionFile();  // pixiv の Cookie

QString libraryRoot();  // 作品の保存先 (既定: ドキュメント/ScrapeWay)
void setLibraryRoot(const QString& dir);

int defaultWait();
void setDefaultWait(int sec);

}  // namespace paths
