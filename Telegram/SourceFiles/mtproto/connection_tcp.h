/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/auth_key.h"
#include "mtproto/connection_abstract.h"
#include "base/timer.h"

namespace MTP {
namespace internal {

class TcpConnection : public AbstractConnection {
public:
	TcpConnection(
		QThread *thread,
		const ProxyData &proxy);

	ConnectionPointer clone(const ProxyData &proxy) override;

	TimeMs pingTime() const override;
	TimeMs fullConnectTimeout() const override;
	void sendData(mtpBuffer &buffer) override;
	void disconnectFromServer() override;
	void connectToServer(
		const QString &address,
		int port,
		const bytes::vector &protocolSecret,
		int16 protocolDcId) override;
	bool isConnected() const override;

	int32 debugState() const override;

	QString transport() const override;
	QString tag() const override;

private:
	enum class Status {
		Waiting = 0,
		Ready,
		Finished,
	};
	static constexpr auto kShortBufferSize = 65535; // Of ints, 256 kb.

	void socketRead();
	void writeConnectionStart();

	void socketPacket(const char *packet, uint32 length);

	void socketConnected();
	void socketDisconnected();
	void socketError(QAbstractSocket::SocketError e);
	void handleTimeout();

	mtpBuffer handleResponse(const char *packet, uint32 length);
	static void handleError(QAbstractSocket::SocketError e, QTcpSocket &sock);
	static uint32 fourCharsToUInt(char ch1, char ch2, char ch3, char ch4) {
		char ch[4] = { ch1, ch2, ch3, ch4 };
		return *reinterpret_cast<uint32*>(ch);
	}

	void sendBuffer(mtpBuffer &buffer);

	QTcpSocket _socket;
	uint32 _packetIndex = 0; // sent packet number

	uint32 _packetRead = 0;
	uint32 _packetLeft = 0; // reading from socket
	bool _readingToShort = true;
	mtpBuffer _longBuffer;
	mtpPrime _shortBuffer[kShortBufferSize];
	char *_currentPosition = nullptr;

	uchar _sendKey[CTRState::KeySize];
	CTRState _sendState;
	uchar _receiveKey[CTRState::KeySize];
	CTRState _receiveState;
	int16 _protocolDcId = 0;
	bytes::vector _protocolSecret;

	Status _status = Status::Waiting;
	MTPint128 _checkNonce;

	QString _address;
	int32 _port = 0;
	int32 _timeout = 0;
	base::Timer _timeoutTimer;
	TimeMs _pingTime = 0;

};

} // namespace internal
} // namespace MTP
