#include "textcodec.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringDecoder>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace textcodec {
namespace {

// 名前の揺れをまとめる。値は Windows のコードページ (0 = Qt 自身で変換できるもの)
struct Codec {
    QString name;
    unsigned codepage;
};

Codec lookup(const QString& raw)
{
    static const QHash<QString, Codec> table = {
        {"utf-8", {"UTF-8", 0}},          {"utf8", {"UTF-8", 0}},
        {"shift_jis", {"Shift_JIS", 932}}, {"shift-jis", {"Shift_JIS", 932}}, {"sjis", {"Shift_JIS", 932}},
        {"x-sjis", {"Shift_JIS", 932}},    {"ms_kanji", {"Shift_JIS", 932}},  {"windows-31j", {"Shift_JIS", 932}},
        {"cp932", {"Shift_JIS", 932}},     {"csshiftjis", {"Shift_JIS", 932}},
        {"euc-jp", {"EUC-JP", 20932}},     {"x-euc-jp", {"EUC-JP", 20932}},   {"eucjp", {"EUC-JP", 20932}},
        {"iso-2022-jp", {"ISO-2022-JP", 50220}}, {"jis", {"ISO-2022-JP", 50220}},
        {"iso-8859-1", {"windows-1252", 1252}},  {"windows-1252", {"windows-1252", 1252}},
        {"us-ascii", {"windows-1252", 1252}},
        {"euc-kr", {"EUC-KR", 51949}},     {"big5", {"Big5", 950}},
        {"gb2312", {"GBK", 936}},          {"gbk", {"GBK", 936}},
        {"utf-16", {"UTF-16", 0}},         {"utf-16le", {"UTF-16LE", 0}},      {"utf-16be", {"UTF-16BE", 0}},
    };
    return table.value(raw.trimmed().toLower(), Codec{});
}

bool validUtf8(const QByteArray& d)
{
    QStringDecoder dec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString s = dec(d);
    return !dec.hasError();
}

// 不正なバイト並びの数 (少ないほどその文字コードらしい)
int sjisErrors(const QByteArray& d)
{
    int err = 0;
    for (qsizetype i = 0; i < d.size(); ++i) {
        const uchar c = uchar(d[i]);
        if (c < 0x80 || (c >= 0xA1 && c <= 0xDF))
            continue;
        if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
            const uchar t = i + 1 < d.size() ? uchar(d[i + 1]) : 0;
            if ((t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFC))
                ++i;
            else
                ++err;
        } else {
            ++err;
        }
    }
    return err;
}

int eucErrors(const QByteArray& d)
{
    int err = 0;
    for (qsizetype i = 0; i < d.size(); ++i) {
        const uchar c = uchar(d[i]);
        if (c < 0x80)
            continue;
        const uchar t = i + 1 < d.size() ? uchar(d[i + 1]) : 0;
        if ((c >= 0xA1 && c <= 0xFE && t >= 0xA1 && t <= 0xFE) || (c == 0x8E && t >= 0xA1 && t <= 0xDF))
            ++i;
        else if (c == 0x8F && t >= 0xA1 && i + 2 < d.size() && uchar(d[i + 2]) >= 0xA1)
            i += 2;
        else
            ++err;
    }
    return err;
}

QString guess(const QByteArray& d)
{
    if (d.startsWith("\xEF\xBB\xBF"))
        return "UTF-8";
    if (d.contains("\x1B$B") || d.contains("\x1B$@") || d.contains("\x1B(J"))
        return "ISO-2022-JP";
    bool ascii = true;
    for (char c : d)
        if (uchar(c) >= 0x80) {
            ascii = false;
            break;
        }
    if (ascii || validUtf8(d))
        return "UTF-8";
    // 古い個人サイトは Shift_JIS が多いので、同点なら Shift_JIS
    return eucErrors(d) < sjisErrors(d) ? "EUC-JP" : "Shift_JIS";
}

}  // namespace

QString detect(const QByteArray& data, const QString& contentType)
{
    static const QRegularExpression metaRe(
        R"(<meta[^>]+charset\s*=\s*["']?\s*([A-Za-z0-9_\-]+))", QRegularExpression::CaseInsensitiveOption);
    const auto m = metaRe.match(QString::fromLatin1(data.left(8192)));
    if (m.hasMatch() && !lookup(m.captured(1)).name.isEmpty()) {
        const QString name = lookup(m.captured(1)).name;
        // 「中身は Shift_JIS なのに UTF-8 と書いてある」ような食い違いは中身を信じる
        if (name != "UTF-8" || validUtf8(data))
            return name;
    }
    static const QRegularExpression headerRe(R"(charset\s*=\s*["']?([A-Za-z0-9_\-]+))", QRegularExpression::CaseInsensitiveOption);
    const auto h = headerRe.match(contentType);
    if (h.hasMatch()) {
        const QString name = lookup(h.captured(1)).name;
        // サーバーの既定値として付いていた ISO-8859-1 などはあてにならない
        if (!name.isEmpty() && name != "windows-1252" && (name != "UTF-8" || validUtf8(data)))
            return name;
    }
    return guess(data);
}

QString decode(const QByteArray& data, const QString& charset)
{
    const Codec c = lookup(charset);
    if (c.name.isEmpty() || c.codepage == 0) {
        QStringDecoder dec(c.name.isEmpty() ? "UTF-8" : c.name.toLatin1().constData());
        if (dec.isValid())
            return dec(data);
        return QString::fromUtf8(data);
    }
#ifdef Q_OS_WIN
    if (data.isEmpty())
        return {};
    const int n = MultiByteToWideChar(c.codepage, 0, data.constData(), int(data.size()), nullptr, 0);
    if (n > 0) {
        QString out(n, Qt::Uninitialized);
        MultiByteToWideChar(c.codepage, 0, data.constData(), int(data.size()), reinterpret_cast<wchar_t*>(out.data()), n);
        return out;
    }
#endif
    QStringDecoder dec(c.name.toLatin1().constData());
    return dec.isValid() ? dec(data) : QString::fromLatin1(data);
}

}  // namespace textcodec
