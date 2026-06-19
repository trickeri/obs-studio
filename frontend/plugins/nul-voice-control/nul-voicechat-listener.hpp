/*
    Nuldrums / NulStream: voicechat transcript listener.
    SPDX-License-Identifier: GPL-2.0-or-later

    Connects to the universal voicechat dictation daemon's Unix socket and emits
    a signal for each finished transcript. Mirrors the Kdenlive/Krita listeners
    (see ~/programming/OBS/AIPlans/voicechat-integration.md).
*/

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QLocalSocket;
class QTimer;

/* Read-only, best-effort listener: if voicechat isn't running it just keeps
 * retrying every ~2s and NulStream is unaffected. */
class NulVoiceChatListener : public QObject {
	Q_OBJECT
public:
	explicit NulVoiceChatListener(QObject *parent = nullptr);

	/* Transcript socket path: $VOICECHAT_SOCKET, else $XDG_RUNTIME_DIR/voicechat.sock. */
	static QString socketPath();

signals:
	/* Emitted for each transcript. mode is voicechat's routing mode (act on "emit"). */
	void transcriptReceived(const QString &text, const QString &app, const QString &mode);

private slots:
	void onReadyRead();
	void scheduleReconnect();
	void tryConnect();

private:
	QLocalSocket *m_socket;
	QTimer *m_reconnect;
	QByteArray m_buffer;
};
