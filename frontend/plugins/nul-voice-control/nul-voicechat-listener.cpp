/*
    Nuldrums / NulStream: voicechat transcript listener.
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "nul-voicechat-listener.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QTimer>

#include <unistd.h>

NulVoiceChatListener::NulVoiceChatListener(QObject *parent)
	: QObject(parent),
	  m_socket(new QLocalSocket(this)),
	  m_reconnect(new QTimer(this))
{
	m_reconnect->setSingleShot(true);
	m_reconnect->setInterval(2000);
	connect(m_reconnect, &QTimer::timeout, this, &NulVoiceChatListener::tryConnect);

	connect(m_socket, &QLocalSocket::readyRead, this, &NulVoiceChatListener::onReadyRead);
	connect(m_socket, &QLocalSocket::disconnected, this, &NulVoiceChatListener::scheduleReconnect);
	connect(m_socket, &QLocalSocket::errorOccurred, this,
		[this](QLocalSocket::LocalSocketError) { scheduleReconnect(); });

	tryConnect();
}

QString NulVoiceChatListener::socketPath()
{
	const QByteArray path = qgetenv("VOICECHAT_SOCKET");
	if (!path.isEmpty())
		return QString::fromLocal8Bit(path);

	QByteArray runtimeDir = qgetenv("XDG_RUNTIME_DIR");
	if (runtimeDir.isEmpty())
		runtimeDir = QByteArrayLiteral("/run/user/") + QByteArray::number(static_cast<qlonglong>(getuid()));
	return QString::fromLocal8Bit(runtimeDir) + QStringLiteral("/voicechat.sock");
}

void NulVoiceChatListener::tryConnect()
{
	if (m_socket->state() != QLocalSocket::UnconnectedState)
		return;
	m_socket->connectToServer(socketPath(), QIODevice::ReadOnly);
}

void NulVoiceChatListener::scheduleReconnect()
{
	m_buffer.clear();
	if (!m_reconnect->isActive())
		m_reconnect->start();
}

void NulVoiceChatListener::onReadyRead()
{
	m_buffer.append(m_socket->readAll());
	int newline;
	while ((newline = m_buffer.indexOf('\n')) >= 0) {
		const QByteArray line = m_buffer.left(newline);
		m_buffer.remove(0, newline + 1);
		if (line.trimmed().isEmpty())
			continue;

		QJsonParseError err;
		const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
		if (err.error != QJsonParseError::NoError || !doc.isObject())
			continue;

		const QJsonObject obj = doc.object();
		const QString text = obj.value(QStringLiteral("text")).toString();
		if (text.isEmpty())
			continue;

		emit transcriptReceived(text, obj.value(QStringLiteral("app")).toString(),
					obj.value(QStringLiteral("mode")).toString());
	}
}
