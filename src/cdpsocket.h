#pragma once

#include <QByteArray>
#include <QObject>
#include <QTcpSocket>
#include <QUrl>

// Chrome DevTools (ws://127.0.0.1:<port>/...) と話すための最小限の WebSocket クライアント。
// 対応範囲: 暗号化なし (ws://)・テキストフレーム・分割フレーム・ping/pong・close。
// Qt WebSockets モジュールが入っていない Qt でもビルドできるようにするため自前で持つ
class CdpSocket : public QObject {
    Q_OBJECT
public:
    explicit CdpSocket(QObject* parent = nullptr);
    void open(const QUrl& url);
    void sendTextMessage(const QString& text);
    void close();

signals:
    void connected();
    void textMessageReceived(const QString& text);
    void disconnected();

private:
    void onReadyRead();
    bool parseHandshake();
    void parseFrames();
    void sendFrame(quint8 opcode, const QByteArray& payload);

    QTcpSocket socket_;
    QUrl url_;
    QByteArray key_, buffer_, message_;
    bool open_ = false;
};
