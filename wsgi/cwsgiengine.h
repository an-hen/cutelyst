/*
 * Copyright (C) 2016-2017 Daniel Nicoletti <dantti12@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB. If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
#ifndef CWSGI_ENGINE_H
#define CWSGI_ENGINE_H

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>

#include <Cutelyst/Engine>

class QTcpServer;

namespace CWSGI {

class TcpServer;

class WSGI;
class CWsgiEngine : public Cutelyst::Engine
{
    Q_OBJECT
public:
    CWsgiEngine(Cutelyst::Application *localApp, int workerCore, const QVariantMap &opts, WSGI *wsgi);

    virtual int workerId() const override;

    inline void processSocket(Cutelyst::EngineRequest *sock) {
        processRequest(*sock);
    }

    void setServers(const std::vector<QObject *> &servers);

    void postFork(int workerId);

    int m_workerId = 0;

    virtual bool init() override;

Q_SIGNALS:
    void started();
    void shutdown();
    void shutdownCompleted(CWsgiEngine *engine);

protected:
    virtual bool finalizeHeadersWrite(Cutelyst::Context *c, quint16 status,  const Cutelyst::Headers &headers, void *engineData) override;

    virtual qint64 doWrite(Cutelyst::Context *c, const char *data, qint64 len, void *engineData) override;

    virtual bool websocketHandshakeDo(Cutelyst::Context *c, const QString &key, const QString &origin, const QString &protocol, void *engineData) override;

    inline void startSocketTimeout() {
        if (m_socketTimeout && ++m_serversTimeout == 1) {
            m_socketTimeout->start();
        }
    }

    inline void stopSocketTimeout() {
        if (m_socketTimeout && --m_serversTimeout == 0) {
            m_socketTimeout->stop();
        }
    }

    inline void serverShutdown() {
        if (--m_runningServers == 0) {
            Q_EMIT shutdownCompleted(this);
        }
    }

private:
    friend class ProtocolHttp;
    friend class ProtocolFastCGI;
    friend class LocalServer;
    friend class TcpServer;
    friend class TcpSslServer;

    QByteArray m_lastDate;
    QElapsedTimer m_lastDateTimer;
    QTimer *m_socketTimeout = nullptr;
    WSGI *m_wsgi;
    int m_runningServers = 0;
    int m_serversTimeout = 0;
};

}

#endif // CWSGI_ENGINE_H
