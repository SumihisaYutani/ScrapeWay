#include "cdpsocket.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QtEndian>

#include <utility>

namespace {

enum Opcode : quint8 { Continuation = 0x0, Text = 0x1, Binary = 0x2, Close = 0x8, Ping = 0x9, Pong = 0xA };

QByteArray randomBytes(int n)
{
    QByteArray b(n, Qt::Uninitialized);
    for (auto& c : b)
        c = char(QRandomGenerator::global()->bounded(256));
    return b;
}

}  // namespace

CdpSocket::CdpSocket(QObject* parent) : QObject(parent)
{
    connect(&socket_, &QTcpSocket::connected, this, [this] {
        key_ = randomBytes(16).toBase64();
        const QByteArray path = url_.path(QUrl::FullyEncoded).toUtf8() +
                                (url_.hasQuery() ? "?" + url_.query(QUrl::FullyEncoded).toUtf8() : QByteArray());
        const QByteArray host = url_.host().toUtf8() + ":" + QByteArray::number(url_.port(80));
        socket_.write("GET " + (path.isEmpty() ? QByteArray("/") : path) + " HTTP/1.1\r\n"
                      "Host: " + host + "\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: " + key_ + "\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n");
    });
    connect(&socket_, &QTcpSocket::readyRead, this, &CdpSocket::onReadyRead);
    connect(&socket_, &QTcpSocket::disconnected, this, [this] {
        const bool was = open_;
        open_ = false;
        if (was)
            emit disconnected();
    });
}

void CdpSocket::open(const QUrl& url)
{
    url_ = url;
    open_ = false;
    buffer_.clear();
    message_.clear();
    socket_.abort();
    socket_.connectToHost(url.host(), quint16(url.port(80)));
}

void CdpSocket::sendTextMessage(const QString& text)
{
    if (open_)
        sendFrame(Text, text.toUtf8());
}

void CdpSocket::close()
{
    if (open_)
        sendFrame(Close, {});
    open_ = false;
    socket_.disconnectFromHost();
}

void CdpSocket::onReadyRead()
{
    buffer_ += socket_.readAll();
    if (!open_ && !parseHandshake())
        return;
    parseFrames();
}

// "HTTP/1.1 101 Switching Protocols" と Sec-WebSocket-Accept を確かめる
bool CdpSocket::parseHandshake()
{
    const qsizetype end = buffer_.indexOf("\r\n\r\n");
    if (end < 0)
        return false;
    const QList<QByteArray> lines = buffer_.left(end).split('\n');
    buffer_.remove(0, end + 4);

    const QByteArray expected =
        QCryptographicHash::hash(key_ + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();
    bool accepted = false;
    for (const auto& l : lines) {
        const QByteArray line = l.trimmed();
        if (line.toLower().startsWith("sec-websocket-accept:"))
            accepted = line.mid(line.indexOf(':') + 1).trimmed() == expected;
    }
    if (lines.isEmpty() || !lines.first().contains(" 101") || !accepted) {
        socket_.abort();
        return false;
    }
    open_ = true;
    emit connected();
    return true;
}

void CdpSocket::parseFrames()
{
    while (open_ && buffer_.size() >= 2) {
        const auto* p = reinterpret_cast<const uchar*>(buffer_.constData());
        const bool fin = p[0] & 0x80;
        const quint8 opcode = p[0] & 0x0F;
        const bool masked = p[1] & 0x80;
        quint64 len = p[1] & 0x7F;
        qsizetype pos = 2;
        if (len == 126) {
            if (buffer_.size() < 4)
                return;
            len = qFromBigEndian<quint16>(p + 2);
            pos = 4;
        } else if (len == 127) {
            if (buffer_.size() < 10)
                return;
            len = qFromBigEndian<quint64>(p + 2);
            pos = 10;
        }
        const qsizetype maskPos = pos;
        if (masked)
            pos += 4;
        if (quint64(buffer_.size() - pos) < len)
            return;

        QByteArray payload = buffer_.mid(pos, qsizetype(len));
        if (masked)
            for (qsizetype i = 0; i < payload.size(); ++i)
                payload[i] = char(payload[i] ^ buffer_[maskPos + (i % 4)]);
        buffer_.remove(0, pos + qsizetype(len));

        switch (opcode) {
        case Text:
        case Binary:
        case Continuation:
            message_ += payload;
            if (fin) {
                const QByteArray msg = std::exchange(message_, {});
                emit textMessageReceived(QString::fromUtf8(msg));
            }
            break;
        case Ping:
            sendFrame(Pong, payload);
            break;
        case Close:
            close();
            return;
        default:
            break;
        }
    }
}

// クライアントから送るフレームは必ずマスクする (RFC 6455)
void CdpSocket::sendFrame(quint8 opcode, const QByteArray& payload)
{
    QByteArray frame;
    frame.append(char(0x80 | opcode));
    const quint64 len = quint64(payload.size());
    if (len < 126) {
        frame.append(char(0x80 | len));
    } else if (len <= 0xFFFF) {
        frame.append(char(0x80 | 126));
        uchar b[2];
        qToBigEndian<quint16>(quint16(len), b);
        frame.append(reinterpret_cast<const char*>(b), 2);
    } else {
        frame.append(char(0x80 | 127));
        uchar b[8];
        qToBigEndian<quint64>(len, b);
        frame.append(reinterpret_cast<const char*>(b), 8);
    }
    const QByteArray mask = randomBytes(4);
    frame.append(mask);
    for (qsizetype i = 0; i < payload.size(); ++i)
        frame.append(char(payload[i] ^ mask[i % 4]));
    socket_.write(frame);
}
