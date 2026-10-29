#include "local_tile_server.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QDebug>

namespace jlusv {

LocalTileServer::LocalTileServer(quint16 port, QObject *parent)
    : QObject(parent)
    , m_port(port)
    , m_server(new QTcpServer(this))
{
    const QString appDir = QCoreApplication::applicationDirPath();
    m_searchDirs << appDir + "/maps/tiles"
                 << appDir + "/../maps/tiles"
                 << appDir + "/../../maps/tiles"
                 << QDir::currentPath() + "/maps/tiles"
                 << "maps/tiles";
}

LocalTileServer::~LocalTileServer() {
    if (m_server->isListening()) {
        m_server->close();
    }
}

bool LocalTileServer::start() {
    connect(m_server, &QTcpServer::newConnection, this, &LocalTileServer::onNewConnection);
    if (!m_server->listen(QHostAddress::LocalHost, m_port)) {
        qWarning() << "LocalTileServer failed to listen on port" << m_port << ":" << m_server->errorString();
        return false;
    }
    qInfo() << "LocalTileServer listening on http://127.0.0.1:" << m_port;
    return true;
}

void LocalTileServer::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, &LocalTileServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    }
}

void LocalTileServer::onReadyRead() {
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket) return;

    if (socket->canReadLine()) {
        const QString line = QString::fromUtf8(socket->readLine()).trimmed();
        handleRequest(socket, line);
    }
}

void LocalTileServer::handleRequest(QTcpSocket *socket, const QString &requestLine) {
    // 匹配请求格式，例如: GET /17/111151/47748.png HTTP/1.1 或 GET /tiles/17/111151/47748.png
    static const QRegularExpression regex(R"(GET\s+(?:.*?)/(\d+)/(\d+)/(\d+)(?:\.png|\.jpg)?(?:\?.*)?\s+HTTP)");
    const auto match = regex.match(requestLine);

    if (!match.hasMatch()) {
        sendNotFound(socket);
        return;
    }

    const QString z = match.captured(1);
    const QString x = match.captured(2);
    const QString y = match.captured(3);

    // 搜索本地瓦片目录
    for (const QString &baseDir : m_searchDirs) {
        const QString filePath = QString("%1/%2/%3/%4.png").arg(baseDir, z, x, y);
        if (QFile::exists(filePath)) {
            QFile file(filePath);
            if (file.open(QIODevice::ReadOnly)) {
                sendResponse(socket, file.readAll(), "image/png");
                return;
            }
        }
    }

    // 未找到对应瓦片时返回 404
    sendNotFound(socket);
}

void LocalTileServer::sendResponse(QTcpSocket *socket, const QByteArray &data, const QString &contentType) {
    QByteArray header;
    header += "HTTP/1.1 200 OK\r\n";
    header += "Content-Type: " + contentType.toUtf8() + "\r\n";
    header += "Content-Length: " + QByteArray::number(data.size()) + "\r\n";
    header += "Access-Control-Allow-Origin: *\r\n";
    header += "Connection: close\r\n\r\n";

    socket->write(header);
    socket->write(data);
    socket->flush();
    socket->disconnectFromHost();
}

void LocalTileServer::sendNotFound(QTcpSocket *socket) {
    const QByteArray body = "Tile not found";
    QByteArray header;
    header += "HTTP/1.1 404 Not Found\r\n";
    header += "Content-Type: text/plain\r\n";
    header += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    header += "Connection: close\r\n\r\n";

    socket->write(header);
    socket->write(body);
    socket->flush();
    socket->disconnectFromHost();
}

} // namespace jlusv
