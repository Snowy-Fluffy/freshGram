// This is the source code of freshGram.
#pragma once

#include <QtCore/QString>
#include <QtNetwork/QHostAddress>

namespace FreshPlugins::Sock {

// A socket descriptor, SOCKET on Windows.
using Fd = qintptr;
constexpr auto kInvalid = Fd(-1);

struct Opened {
	Fd fd = kInvalid;
	int error = 0;
	bool badAddress = false;
};

// Starts a non-blocking connection, the descriptor is not inherited by children.
[[nodiscard]] Opened BeginConnect(const QHostAddress &address, int port);

// 1 when the socket is ready, 0 on a timeout or a signal, -1 on a failure.
[[nodiscard]] int Wait(Fd fd, bool forWrite, int timeoutMs);
// The result of a started connection: 0 when it is established.
[[nodiscard]] int PendingError(Fd fd);

constexpr auto kWouldBlock = -2;
// The byte count, kWouldBlock to try again later, -1 on a failure.
[[nodiscard]] int Send(Fd fd, const char *data, size_t size);
// The byte count, 0 when the peer closed, kWouldBlock or -1 as for Send.
[[nodiscard]] int Recv(Fd fd, char *out, size_t size);

void Close(Fd fd);

[[nodiscard]] int LastError();
[[nodiscard]] QString ErrorText(int code);

} // namespace FreshPlugins::Sock
