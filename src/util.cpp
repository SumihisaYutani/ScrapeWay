#include "util.h"

#include "net.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QSaveFile>

namespace util {

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString& path, const QByteArray& data)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
        throw net::Error(QString("書き込みに失敗しました: %1").arg(QDir::toNativeSeparators(path)));
}

QString replaceEach(const QString& s, const QRegularExpression& re,
                    const std::function<QString(const QRegularExpressionMatch&)>& f)
{
    QString out;
    qsizetype last = 0;
    auto it = re.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        out += QStringView(s).mid(last, m.capturedStart() - last);
        out += f(m);
        last = m.capturedEnd();
    }
    out += QStringView(s).mid(last);
    return out;
}

QString htmlToText(QString s)
{
    s.replace(QRegularExpression(R"(<br\s*/?>)", QRegularExpression::CaseInsensitiveOption), "\n");
    s.remove(QRegularExpression("<[^>]+>"));
    static const QRegularExpression entity(R"(&(#[xX][0-9a-fA-F]+|#\d+|amp|lt|gt|quot|apos|nbsp);)");
    return replaceEach(s, entity, [](const QRegularExpressionMatch& m) -> QString {
        const QString e = m.captured(1);
        if (e.startsWith('#')) {
            bool ok = false;
            const uint cp = (e.size() > 1 && (e[1] == 'x' || e[1] == 'X')) ? e.mid(2).toUInt(&ok, 16) : e.mid(1).toUInt(&ok);
            if (!ok || cp == 0 || cp > 0x10FFFF)
                return m.captured(0);
            const char32_t c = cp;
            return QString::fromUcs4(&c, 1);
        }
        static const QHash<QString, QString> named = {
            {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "}};
        return named.value(e);
    });
}

}  // namespace util
