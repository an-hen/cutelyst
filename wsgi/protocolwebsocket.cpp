/*
 * Copyright (C) 2017 Daniel Nicoletti <dantti12@gmail.com>
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
#include "protocolwebsocket.h"

#include "socket.h"
#include "wsgi.h"

#include <Cutelyst/Headers>
#include <Cutelyst/Context>
#include <Cutelyst/Response>

#include <QLoggingCategory>

#include <QTextCodec>

using namespace CWSGI;

Q_LOGGING_CATEGORY(CWSGI_WS, "cwsgi.websocket")

ProtocolWebSocket::ProtocolWebSocket(CWSGI::WSGI *wsgi) : Protocol(wsgi)
  , m_codec(QTextCodec::codecForName(QByteArrayLiteral("UTF-8")))
{
    m_websockets_max_size = wsgi->websocketMaxSize() * 1024;
}

ProtocolWebSocket::~ProtocolWebSocket()
{
}

QByteArray ProtocolWebSocket::createWebsocketHeader(quint8 opcode, quint64 len)
{
    QByteArray ret;
    ret.append(0x80 + opcode);

    if (len < 126) {
        ret.append(static_cast<quint8>(len));
    } else if (len <= static_cast<quint16>(0xffff)) {
        ret.append(126);

        quint8 buf[2];
        buf[1] = (quint8) (len & 0xff);
        buf[0] = (quint8) ((len >> 8) & 0xff);
        ret.append(reinterpret_cast<char*>(buf), 2);
    } else {
        ret.append(127);

        quint8 buf[8];
        buf[7] = (quint8) (len & 0xff);
        buf[6] = (quint8) ((len >> 8) & 0xff);
        buf[5] = (quint8) ((len >> 16) & 0xff);
        buf[4] = (quint8) ((len >> 24) & 0xff);
        buf[3] = (quint8) ((len >> 32) & 0xff);
        buf[2] = (quint8) ((len >> 40) & 0xff);
        buf[1] = (quint8) ((len >> 48) & 0xff);
        buf[0] = (quint8) ((len >> 56) & 0xff);
        ret.append(reinterpret_cast<char*>(buf), 8);
    }

    return ret;
}

QByteArray ProtocolWebSocket::createWebsocketCloseReply(const QString &msg, quint16 closeCode)
{
    QByteArray payload;

    const QByteArray data = msg.toUtf8().left(123);

    payload = ProtocolWebSocket::createWebsocketHeader(Socket::OpCodeClose, data.size() + 2);

    quint8 buf[2];
    buf[1] = (quint8) (closeCode & 0xff);
    buf[0] = (quint8) ((closeCode >> 8) & 0xff);
    payload.append(reinterpret_cast<char*>(buf), 2);

    // 125 is max payload - 2 of the above bytes
    payload.append(data);

    return payload;
}

quint16 ws_be16(const char *buf) {
    const quint32 *src = reinterpret_cast<const quint32 *>(buf);
    quint16 ret = 0;
    quint8 *ptr = reinterpret_cast<quint8 *>(&ret);
    ptr[0] = static_cast<quint8>((*src >> 8) & 0xff);
    ptr[1] = static_cast<quint8>(*src & 0xff);
    return ret;
}

quint64 ws_be64(const char *buf) {
    const quint64 *src = reinterpret_cast<const quint64 *>(buf);
    quint64 ret = 0;
    quint8 *ptr = reinterpret_cast<quint8 *>(&ret);
    ptr[0] = static_cast<quint8>((*src >> 56) & 0xff);
    ptr[1] = static_cast<quint8>((*src >> 48) & 0xff);
    ptr[2] = static_cast<quint8>((*src >> 40) & 0xff);
    ptr[3] = static_cast<quint8>((*src >> 32) & 0xff);
    ptr[4] = static_cast<quint8>((*src >> 24) & 0xff);
    ptr[5] = static_cast<quint8>((*src >> 16) & 0xff);
    ptr[6] = static_cast<quint8>((*src >> 8) & 0xff);
    ptr[7] = static_cast<quint8>(*src & 0xff);
    return ret;
}

void ProtocolWebSocket::readyRead(Socket *sock, QIODevice *io) const
{
    qint64 bytesAvailable = io->bytesAvailable();

    Q_FOREVER {
        if (!bytesAvailable || !sock->websocket_need || (bytesAvailable < sock->websocket_need && sock->websocket_phase != Socket::WebSocketPhasePayload)) {
            // Need more data
            return;
        }

        quint32 maxlen = qMin(sock->websocket_need, static_cast<quint32>(m_postBufferSize));
        qint64 len = io->read(m_postBuffer, maxlen);
        if (len == -1) {
            qCWarning(CWSGI_WS) << "Failed to read from socket" << io->errorString();
            sock->connectionClose();
            return;
        }
        bytesAvailable -= len;

        switch(sock->websocket_phase) {
        case Socket::WebSocketPhaseHeaders:
            if (!websocket_parse_header(sock, m_postBuffer, io)) {
                return;
            }
            break;
        case Socket::WebSocketPhaseSize:
            if (!websocket_parse_size(sock, m_postBuffer, m_websockets_max_size)) {
                return;
            }
            break;
        case Socket::WebSocketPhaseMask:
            websocket_parse_mask(sock, m_postBuffer, io);
            break;
        case Socket::WebSocketPhasePayload:
            if (!websocket_parse_payload(sock, m_postBuffer, len, io)) {
                return;
            }
            break;
        }
    }
}

bool ProtocolWebSocket::sendHeaders(QIODevice *io, Socket *sock, quint16 status, const QByteArray &dateHeader, const Cutelyst::Headers &headers)
{
    Q_UNUSED(io)
    Q_UNUSED(sock)
    Q_UNUSED(status)
    Q_UNUSED(dateHeader)
    Q_UNUSED(headers)
    qFatal("ProtocolWebSocket::sendHeaders() called!");
    return false;
}

bool ProtocolWebSocket::send_text(Cutelyst::Context *c, Socket *sock, bool singleFrame) const
{
    Cutelyst::Request *request = c->request();

    const int msg_size = sock->websocket_message.size();
    sock->websocket_message.append(sock->websocket_payload);

    QByteArray payload = sock->websocket_payload;
    if (sock->websocket_start_of_frame != msg_size) {
        payload = sock->websocket_message.mid(sock->websocket_start_of_frame);
    }

    QTextCodec::ConverterState state;
    const QString frame = m_codec->toUnicode(payload.data(), payload.size(), &state);
    const bool failed = state.invalidChars || state.remainingChars;
    if (singleFrame && (failed || (frame.isEmpty() && payload.size()))) {
        sock->connectionClose();
        return false;
    } else if (!failed) {
        sock->websocket_start_of_frame = sock->websocket_message.size();
        request->webSocketTextFrame(frame,
                                    sock->websocket_finn_opcode & 0x80,
                                    sock->websocketContext);
    }

    if (sock->websocket_finn_opcode & 0x80) {
        sock->websocket_continue_opcode = 0;
        if (singleFrame || sock->websocket_payload == sock->websocket_message) {
            request->webSocketTextMessage(frame,
                                          sock->websocketContext);
        } else {
            QTextCodec::ConverterState stateMsg;
            const QString msg = m_codec->toUnicode(sock->websocket_message.data(), sock->websocket_message.size(), &stateMsg);
            const bool failed = state.invalidChars || state.remainingChars;
            if (failed) {
                sock->connectionClose();
                return false;
            }
            request->webSocketTextMessage(msg,
                                          sock->websocketContext);
        }
        sock->websocket_message = QByteArray();
        sock->websocket_payload = QByteArray();
    }

    return true;
}

void ProtocolWebSocket::send_binary(Cutelyst::Context *c, Socket *sock, bool singleFrame) const
{
    Cutelyst::Request *request = c->request();

    sock->websocket_message.append(sock->websocket_payload);

    const QByteArray frame = sock->websocket_payload;
    request->webSocketBinaryFrame(frame,
                                  sock->websocket_finn_opcode & 0x80,
                                  sock->websocketContext);

    if (sock->websocket_finn_opcode & 0x80) {
        sock->websocket_continue_opcode = 0;
        if (singleFrame || sock->websocket_payload == sock->websocket_message) {
            request->webSocketBinaryMessage(frame,
                                            sock->websocketContext);
        } else {
            request->webSocketBinaryMessage(sock->websocket_message,
                                            sock->websocketContext);
        }
        sock->websocket_message = QByteArray();
        sock->websocket_payload = QByteArray();
    }
}

void ProtocolWebSocket::send_pong(QIODevice *io, const QByteArray data) const
{
    io->write(ProtocolWebSocket::createWebsocketHeader(Socket::OpCodePong, data.size()));
    io->write(data);
}

void ProtocolWebSocket::send_closed(Cutelyst::Context *c, Socket *sock, QIODevice *io) const
{
    quint16 closeCode = Cutelyst::Response::CloseCodeMissingStatusCode;
    QString reason;
    QTextCodec::ConverterState state;
    if (sock->websocket_payload.size() >= 2) {
        closeCode = ws_be16(sock->websocket_payload.data());
        reason = m_codec->toUnicode(sock->websocket_payload.data() + 2, sock->websocket_payload.size() - 2, &state);
    }
    c->request()->webSocketClosed(closeCode, reason);

    if (state.invalidChars || state.remainingChars) {
        reason = QString();
        closeCode = Cutelyst::Response::CloseCodeProtocolError;
    } else if (closeCode < 3000 || closeCode > 4999) {
        switch (closeCode) {
        case Cutelyst::Response::CloseCodeNormal:
        case Cutelyst::Response::CloseCodeGoingAway:
        case Cutelyst::Response::CloseCodeProtocolError:
        case Cutelyst::Response::CloseCodeDatatypeNotSupported:
            //    case Cutelyst::Response::CloseCodeReserved1004:
            break;
        case Cutelyst::Response::CloseCodeMissingStatusCode:
            if (sock->websocket_payload.isEmpty()) {
                closeCode = Cutelyst::Response::CloseCodeNormal;
            } else {
                closeCode = Cutelyst::Response::CloseCodeProtocolError;
            }
            break;
            //    case Cutelyst::Response::CloseCodeAbnormalDisconnection:
        case Cutelyst::Response::CloseCodeWrongDatatype:
        case Cutelyst::Response::CloseCodePolicyViolated:
        case Cutelyst::Response::CloseCodeTooMuchData:
        case Cutelyst::Response::CloseCodeMissingExtension:
        case Cutelyst::Response::CloseCodeBadOperation:
            //    case Cutelyst::Response::CloseCodeTlsHandshakeFailed:
            break;
        default:
            reason = QString();
            closeCode = Cutelyst::Response::CloseCodeProtocolError;
            break;
        }
    }

    const QByteArray reply = ProtocolWebSocket::createWebsocketCloseReply(reason, closeCode);
    io->write(reply);

    sock->connectionClose();
}

bool ProtocolWebSocket::websocket_parse_header(Socket *sock, const char *buf, QIODevice *io) const
{
    quint8 byte1 = buf[0];
    quint8 byte2 = buf[1];

    sock->websocket_finn_opcode = byte1;
    sock->websocket_payload_size = byte2 & 0x7f;

    quint8 opcode = byte1 & 0xf;

    bool websocket_has_mask = byte2 >> 7;
    if (!websocket_has_mask ||
            ((opcode == Socket::OpCodePing || opcode == Socket::OpCodeClose) && sock->websocket_payload_size > 125) ||
            (byte1 & 0x70) ||
            ((opcode >= Socket::OpCodeReserved3 && opcode <= Socket::OpCodeReserved7) ||
             (opcode >= Socket::OpCodeReservedB && opcode <= Socket::OpCodeReservedF)) ||
            (!(byte1 & 0x80) && opcode != Socket::OpCodeText && opcode != Socket::OpCodeBinary && opcode != Socket::OpCodeContinue) ||
            (sock->websocket_continue_opcode && (opcode == Socket::OpCodeText || opcode == Socket::OpCodeBinary))) {
        // RFC errors
        // client to server MUST have a mask
        // Control opcode cannot have payload bigger than 125
        // RSV bytes MUST not be set
        // reserved opcodes must not be set 3-7
        // reserved opcodes must not be set B-F
        // Only Text/Bynary/Coninue opcodes can be fragmented
        // Continue opcode was set but was NOT followed by CONTINUE

        io->write(ProtocolWebSocket::createWebsocketCloseReply(QString(), 1002)); // Protocol error
        sock->connectionClose();
        return false;
    }

    if (opcode == Socket::OpCodeText || opcode == Socket::OpCodeBinary) {
        sock->websocket_message = QByteArray();
        sock->websocket_start_of_frame = 0;
        if (!(byte1 & 0x80)) {
            // FINN byte not set, store opcode for continue
            sock->websocket_continue_opcode = opcode;
        }
    }

    if (sock->websocket_payload_size == 126) {
        sock->websocket_need = 2;
        sock->websocket_phase = Socket::WebSocketPhaseSize;
    } else if (sock->websocket_payload_size == 127) {
        sock->websocket_need = 8;
        sock->websocket_phase = Socket::WebSocketPhaseSize;
    } else {
        sock->websocket_need = 4;
        sock->websocket_phase = Socket::WebSocketPhaseMask;
    }

    return true;
}

bool ProtocolWebSocket::websocket_parse_size(Socket *sock, const char *buf, int websockets_max_message_size) const
{
    quint64 size;
    if (sock->websocket_payload_size == 126) {
        size = ws_be16(buf);
    } else if (sock->websocket_payload_size == 127) {
        size = ws_be64(buf);
    } else {
        qCCritical(CWSGI_WS) << "BUG error in websocket parser:" << sock->websocket_payload_size;
        sock->connectionClose();
        return false;
    }

    if (size > static_cast<quint64>(websockets_max_message_size)) {
        qCCritical(CWSGI_WS) << "Payload size too big" << size << "max allowed" << websockets_max_message_size;
        sock->connectionClose();
        return false;
    }
    sock->websocket_payload_size = size;

    sock->websocket_need = 4;
    sock->websocket_phase = Socket::WebSocketPhaseMask;

    return true;
}

void ProtocolWebSocket::websocket_parse_mask(Socket *sock, char *buf, QIODevice *io) const
{
    const quint32 *ptr = reinterpret_cast<const quint32 *>(buf);
    sock->websocket_mask = *ptr;

    sock->websocket_phase = Socket::WebSocketPhasePayload;
    sock->websocket_need = sock->websocket_payload_size;

    sock->websocket_payload = QByteArray();
    if (sock->websocket_payload_size == 0) {
        websocket_parse_payload(sock, buf, 0, io);
    } else {
        sock->websocket_payload.reserve(sock->websocket_payload_size);
    }
}

bool ProtocolWebSocket::websocket_parse_payload(Socket *sock, char *buf, uint len, QIODevice *io) const
{
    quint8 *mask = reinterpret_cast<quint8 *>(&sock->websocket_mask);
    for (uint i = 0, maskIx = sock->websocket_payload.size(); i < len; ++i, ++maskIx) {
        buf[i] = buf[i] ^ mask[maskIx % 4];
    }

    sock->websocket_payload.append(buf, len);
    if (sock->websocket_payload.size() < sock->websocket_payload_size) {
        // need more data
        sock->websocket_need -= len;
        return true;
    }

    sock->websocket_need = 2;
    sock->websocket_phase = Socket::WebSocketPhaseHeaders;

    Cutelyst::Request *request = sock->websocketContext->request();

    switch (sock->websocket_finn_opcode & 0xf) {
    case Socket::OpCodeContinue:
        switch (sock->websocket_continue_opcode) {
        case Socket::OpCodeText:
            if (!send_text(sock->websocketContext, sock, false)) {
                return false;
            }
            break;
        case Socket::OpCodeBinary:
            send_binary(sock->websocketContext, sock, false);
            break;
        default:
            qCCritical(CWSGI_WS) << "Invalid CONTINUE opcode:" << (sock->websocket_finn_opcode & 0xf);
            sock->connectionClose();
            return false;
        }
        break;
    case Socket::OpCodeText:
        if (!send_text(sock->websocketContext, sock, sock->websocket_finn_opcode & 0x80)) {
            return false;
        }
        break;
    case Socket::OpCodeBinary:
        send_binary(sock->websocketContext, sock, sock->websocket_finn_opcode & 0x80);
        break;
    case Socket::OpCodeClose:
        send_closed(sock->websocketContext, sock, io);
        return false;
    case Socket::OpCodePing:
        send_pong(io, sock->websocket_payload.left(125));
        break;
    case Socket::OpCodePong:
        request->webSocketPong(sock->websocket_payload,
                               sock->websocketContext);
        break;
    default:
        break;
    }

    return true;
}
