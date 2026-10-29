#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QStringList>

namespace jlusv {

class LocalTileServer : public QObject {
    Q_OBJECT
public:
    explicit LocalTileServer(quint16 port = 18080, QObject *parent = nullptr);
    ~LocalTileServer() override;

    bool start();
    quint16 port() const { return m_port; }

private slots:
    void onNewConnection();
    void onReadyRead();

private:
    void handleRequest(QTcpSocket *socket, const QString &requestLine);
    void sendResponse(QTcpSocket *socket, const QByteArray &data, const QString &contentType = "image/png");
    void sendNotFound(QTcpSocket *socket);

    quint16 m_port;
    QTcpServer *m_server;
    QStringList m_searchDirs;
};

} // namespace jlusv
