#pragma once

#include <QByteArray>
#include <QString>

// 古い日本語サイトの HTML を読むための文字コード判定と変換
namespace textcodec {

// <meta> の charset → HTTP ヘッダーの charset → 中身からの推測 の順で決める。
// 戻り値は "UTF-8" "Shift_JIS" "EUC-JP" "ISO-2022-JP" などの正規化した名前
QString detect(const QByteArray& data, const QString& contentType = {});

QString decode(const QByteArray& data, const QString& charset);

}  // namespace textcodec
