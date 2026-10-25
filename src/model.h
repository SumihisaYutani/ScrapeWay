#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

// 取り込み画面に並べる 1 件 (pixiv の 1 話 / Wayback の 1 ページ)
struct Item {
    QString id;  // pixiv: 小説 ID / Wayback: 元の URL
    QString title;
    QString seriesId, seriesTitle;  // 一覧でのまとまり (pixiv: シリーズ / Wayback: リンク元かどうか)
    int order = 0;
    qint64 textCount = 0;  // pixiv: 文字数 / Wayback: おおよそのバイト数
    QString createDate, updateDate;
    bool r18 = false;
    QStringList tags;
    QString status;         // new / saved / updated
    bool preselect = true;  // 未保存なら最初からチェックを入れるか
};

struct ResolveResult {
    QString kind;  // user / series / novel / wayback
    QString id;    // Wayback では取得する時点 (yyyyMMddhhmmss)
    QString title, warning, error;
    QString parentUrl;  // Wayback: 一覧の範囲を一つ上の階層に広げるときの URL
    QList<Item> items;
};

using LogFn = std::function<void(const QString&)>;
