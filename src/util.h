#pragma once

#include <QByteArray>
#include <QRegularExpression>
#include <QString>

#include <functional>

namespace util {

QByteArray readFile(const QString& path);
void writeFile(const QString& path, const QByteArray& data);  // 失敗したら net::Error

// 正規表現に一致した部分を f の戻り値で置き換える
QString replaceEach(const QString& s, const QRegularExpression& re,
                    const std::function<QString(const QRegularExpressionMatch&)>& f);

// HTML 断片を平文にする (<br> は改行、タグは除去、文字参照は戻す)
QString htmlToText(QString s);

}  // namespace util

using util::htmlToText;
using util::readFile;
using util::replaceEach;
using util::writeFile;
