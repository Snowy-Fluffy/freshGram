// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_socket.h"

#ifdef FRESHGRAM_PLUGINS

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else // Q_OS_WIN
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif // Q_OS_WIN

#include <cstring>

namespace FreshPlugins::Sock {
namespace {

#ifdef Q_OS_WIN

[[nodiscard]] SOCKET Native(Fd fd) {
	return SOCKET(fd);
}

[[nodiscard]] bool Pending(int code) {
	return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS;
}

#else // Q_OS_WIN

[[nodiscard]] int Native(Fd fd) {
	return int(fd);
}

[[nodiscard]] bool Pending(int code) {
	return code == EINPROGRESS || code == EAGAIN || code == EINTR;
}

#endif // Q_OS_WIN

} // namespace

Opened BeginConnect(const QHostAddress &address, int port) {
	auto result = Opened();
	auto storage = sockaddr_storage();
	std::memset(&storage, 0, sizeof(storage));
	auto length = 0;
	auto valid = false;
	if (const auto v4 = address.toIPv4Address(&valid); valid) {
		auto &in = reinterpret_cast<sockaddr_in&>(storage);
		in.sin_family = AF_INET;
		in.sin_port = htons(quint16(port));
		in.sin_addr.s_addr = htonl(v4);
		length = int(sizeof(sockaddr_in));
	} else if (address.protocol() == QAbstractSocket::IPv6Protocol) {
		auto &in6 = reinterpret_cast<sockaddr_in6&>(storage);
		in6.sin6_family = AF_INET6;
		in6.sin6_port = htons(quint16(port));
		const auto raw = address.toIPv6Address();
		std::memcpy(&in6.sin6_addr, &raw, sizeof(raw));
		length = int(sizeof(sockaddr_in6));
	} else {
		result.badAddress = true;
		return result;
	}
#ifdef Q_OS_WIN
	// Sockets are not inherited by the children that CreateProcess starts
	// without a handle list, and the plugin host gets an explicit one.
	const auto fd = ::socket(storage.ss_family, SOCK_STREAM, IPPROTO_TCP);
	if (fd == INVALID_SOCKET) {
		result.error = WSAGetLastError();
		return result;
	}
	auto nonblocking = u_long(1);
	if (ioctlsocket(fd, FIONBIO, &nonblocking) != 0) {
		result.error = WSAGetLastError();
		closesocket(fd);
		return result;
	}
	SetHandleInformation(HANDLE(fd), HANDLE_FLAG_INHERIT, 0);
	if (::connect(fd, reinterpret_cast<const sockaddr*>(&storage), length) != 0
		&& !Pending(WSAGetLastError())) {
		result.error = WSAGetLastError();
		closesocket(fd);
		return result;
	}
	result.fd = Fd(fd);
#else // Q_OS_WIN
	const auto fd = ::socket(storage.ss_family, SOCK_STREAM, 0);
	if (fd < 0) {
		result.error = errno;
		return result;
	}
	::fcntl(fd, F_SETFD, FD_CLOEXEC);
	::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#ifdef Q_OS_MAC
	auto yes = 1;
	::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif // Q_OS_MAC
	if (::connect(fd, reinterpret_cast<const sockaddr*>(&storage), socklen_t(length)) != 0
		&& errno != EINPROGRESS) {
		result.error = errno;
		::close(fd);
		return result;
	}
	result.fd = Fd(fd);
#endif // Q_OS_WIN
	return result;
}

int Wait(Fd fd, bool forWrite, int timeoutMs) {
#ifdef Q_OS_WIN
	auto item = WSAPOLLFD{ Native(fd), SHORT(forWrite ? POLLWRNORM : POLLRDNORM), 0 };
	const auto ready = ::WSAPoll(&item, 1, timeoutMs);
	return (ready < 0) ? -1 : (ready == 0 ? 0 : 1);
#else // Q_OS_WIN
	auto item = pollfd{ Native(fd), short(forWrite ? POLLOUT : POLLIN), 0 };
	const auto ready = ::poll(&item, 1, timeoutMs);
	if (ready < 0) {
		return (errno == EINTR) ? 0 : -1;
	}
	return (ready == 0) ? 0 : 1;
#endif // Q_OS_WIN
}

int PendingError(Fd fd) {
	auto error = 0;
#ifdef Q_OS_WIN
	auto size = int(sizeof(error));
	if (::getsockopt(Native(fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) != 0) {
		return WSAGetLastError();
	}
#else // Q_OS_WIN
	auto size = socklen_t(sizeof(error));
	if (::getsockopt(Native(fd), SOL_SOCKET, SO_ERROR, &error, &size) != 0) {
		return errno;
	}
#endif // Q_OS_WIN
	return error;
}

int Send(Fd fd, const char *data, size_t size) {
#ifdef Q_OS_WIN
	const auto written = ::send(Native(fd), data, int(size), 0);
	if (written == SOCKET_ERROR) {
		return Pending(WSAGetLastError()) ? kWouldBlock : -1;
	}
	return written;
#else // Q_OS_WIN
#ifdef Q_OS_MAC
	const auto flags = 0;
#else // Q_OS_MAC
	const auto flags = MSG_NOSIGNAL;
#endif // Q_OS_MAC
	const auto written = ::send(Native(fd), data, size, flags);
	if (written < 0) {
		return Pending(errno) ? kWouldBlock : -1;
	}
	return int(written);
#endif // Q_OS_WIN
}

int Recv(Fd fd, char *out, size_t size) {
#ifdef Q_OS_WIN
	const auto read = ::recv(Native(fd), out, int(size), 0);
	if (read == SOCKET_ERROR) {
		return Pending(WSAGetLastError()) ? kWouldBlock : -1;
	}
	return read;
#else // Q_OS_WIN
	const auto read = ::recv(Native(fd), out, size, 0);
	if (read < 0) {
		return Pending(errno) ? kWouldBlock : -1;
	}
	return int(read);
#endif // Q_OS_WIN
}

void Close(Fd fd) {
#ifdef Q_OS_WIN
	::closesocket(Native(fd));
#else // Q_OS_WIN
	::close(Native(fd));
#endif // Q_OS_WIN
}

int LastError() {
#ifdef Q_OS_WIN
	return WSAGetLastError();
#else // Q_OS_WIN
	return errno;
#endif // Q_OS_WIN
}

QString ErrorText(int code) {
#ifdef Q_OS_WIN
	wchar_t buffer[256] = {};
	const auto size = FormatMessageW(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr,
		DWORD(code),
		0,
		buffer,
		DWORD(std::size(buffer)),
		nullptr);
	return size ? QString::fromWCharArray(buffer, int(size)).trimmed() : QString::number(code);
#else // Q_OS_WIN
	return QString::fromLocal8Bit(std::strerror(code));
#endif // Q_OS_WIN
}

} // namespace FreshPlugins::Sock

#endif // FRESHGRAM_PLUGINS
