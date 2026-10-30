// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_transport.h"

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/broker.h"
#include "fgpl/net_policy.h"
#endif

#include "crl/crl_async.h"

#include <QtCore/QDateTime>
#include <QtCore/QDeadlineTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QHostInfo>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslSocket>
#include <QtNetwork/QUdpSocket>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

#ifdef FRESHGRAM_PLUGINS
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace FreshPlugins {

#ifdef FRESHGRAM_PLUGINS

namespace {

constexpr auto kMaxRedirects = 5;
constexpr auto kMaxHeaderBytes = size_t(64) << 10;
constexpr auto kWaitSliceMs = 50;
constexpr auto kDatagramsPerSecond = 200;
constexpr auto kMaxLogEntries = size_t(300);
constexpr auto kMaxLogPath = 120;

struct Connection {
	int64 id = 0;
	fgpl::NetOwner owner;
	QString kind;
	QString host;
	int port = 0;
	QHostAddress address;
	bool tls = false;
	crl::time opened = 0;
	std::atomic<uint64> received = 0;
	std::atomic<uint64> sent = 0;
	std::atomic<bool> killed = false;
	std::unique_ptr<QTcpSocket> stream;
	std::unique_ptr<QUdpSocket> datagram;
	std::deque<crl::time> recentDatagrams;
	QString via;
	std::unique_ptr<QTcpSocket> control;
	QHostAddress relay;
	int relayPort = 0;
	QByteArray datagramHeader;
};

struct Traffic {
	uint64 received = 0;
	uint64 sent = 0;
};

struct Registry {
	std::mutex mutex;
	std::map<int64, std::shared_ptr<Connection>> connections;
	std::map<std::string, Traffic> finished;
	std::atomic<int64> nextId = 1;
	std::deque<NetworkEvent> log;
	std::atomic<uint64> logVersion = 0;
};

[[nodiscard]] Registry &GetRegistry() {
	static auto result = Registry();
	return result;
}

struct PolicyState {
	std::mutex mutex;
	NetworkPolicy policy;
};

[[nodiscard]] PolicyState &GetPolicyState() {
	static auto result = PolicyState();
	return result;
}

[[nodiscard]] NetworkPolicy CurrentPolicy() {
	auto &state = GetPolicyState();
	const auto lock = std::scoped_lock(state.mutex);
	return state.policy;
}

[[nodiscard]] bool BlockedKind(const NetworkPolicy &policy, const QString &kind) {
	return (kind == u"HTTPS"_q)
		? policy.blockHttps
		: (kind == u"UDP"_q)
		? policy.blockUdp
		: policy.blockTcp;
}

[[nodiscard]] QString ProxyLabel(const NetworkProxy &proxy) {
	return ((proxy.type == NetworkProxy::Type::Socks5)
		? u"socks5 "_q
		: u"http "_q)
		+ QString::fromStdString(proxy.host)
		+ ':'
		+ QString::number(proxy.port);
}

[[nodiscard]] std::shared_ptr<Connection> Register(
		const fgpl::NetOwner &owner,
		QString kind,
		QString host,
		int port) {
	auto &registry = GetRegistry();
	auto connection = std::make_shared<Connection>();
	connection->id = registry.nextId++;
	connection->owner = owner;
	connection->kind = std::move(kind);
	connection->host = std::move(host);
	connection->port = port;
	connection->opened = crl::now();
	const auto lock = std::scoped_lock(registry.mutex);
	registry.connections.emplace(connection->id, connection);
	return connection;
}

void AddLog(NetworkEvent event) {
	auto &registry = GetRegistry();
	event.when = QDateTime::currentMSecsSinceEpoch();
	const auto lock = std::scoped_lock(registry.mutex);
	registry.log.push_back(std::move(event));
	while (registry.log.size() > kMaxLogEntries) {
		registry.log.pop_front();
	}
	++registry.logVersion;
}

[[nodiscard]] NetworkEvent EventOf(const Connection &connection) {
	auto result = NetworkEvent();
	result.plugin = QString::fromStdString(connection.owner.plugin);
	result.kind = connection.kind;
	result.target = connection.host + ':' + QString::number(connection.port);
	result.received = connection.received.load();
	result.sent = connection.sent.load();
	result.duration = crl::now() - connection.opened;
	return result;
}

void Unregister(int64 id) {
	auto &registry = GetRegistry();
	auto connection = std::shared_ptr<Connection>();
	{
		const auto lock = std::scoped_lock(registry.mutex);
		const auto i = registry.connections.find(id);
		if (i == registry.connections.end()) {
			return;
		}
		connection = std::move(i->second);
		registry.connections.erase(i);
		if (connection->stream || connection->datagram) {
			auto event = EventOf(*connection);
			event.action = u"close"_q;
			event.result = connection->killed.load()
				? u"closed by the user"_q
				: QString();
			event.when = QDateTime::currentMSecsSinceEpoch();
			registry.log.push_back(std::move(event));
			while (registry.log.size() > kMaxLogEntries) {
				registry.log.pop_front();
			}
			++registry.logVersion;
		}
		auto &total = registry.finished[connection->owner.plugin];
		total.received += connection->received.load();
		total.sent += connection->sent.load();
	}
	if (connection->stream) {
		connection->stream->abort();
		connection->stream.reset();
	}
	if (connection->datagram) {
		connection->datagram->abort();
		connection->datagram.reset();
	}
	if (connection->control) {
		connection->control->abort();
		connection->control.reset();
	}
}

[[nodiscard]] std::shared_ptr<Connection> Find(int64 id) {
	auto &registry = GetRegistry();
	const auto lock = std::scoped_lock(registry.mutex);
	const auto i = registry.connections.find(id);
	return (i == registry.connections.end()) ? nullptr : i->second;
}

[[nodiscard]] fgpl::FetchError Error(const char *code, QString message) {
	return { code, message.toStdString() };
}

[[nodiscard]] std::string Lower(std::string text) {
	for (auto &ch : text) {
		if (ch >= 'A' && ch <= 'Z') {
			ch = char(ch - 'A' + 'a');
		}
	}
	return text;
}

struct Parsed {
	int status = 0;
	std::vector<std::pair<std::string, std::string>> headers;
	std::string location;
	bool chunked = false;
	std::optional<size_t> length;
	size_t consumed = 0;
};

[[nodiscard]] std::optional<Parsed> ParseHead(const std::string &data) {
	const auto end = data.find("\r\n\r\n");
	if (end == std::string::npos) {
		return std::nullopt;
	}
	auto result = Parsed();
	result.consumed = end + 4;
	auto line = size_t(0);
	auto first = true;
	while (line < end + 2) {
		const auto next = data.find("\r\n", line);
		const auto text = data.substr(line, next - line);
		line = next + 2;
		if (first) {
			first = false;
			if (text.rfind("HTTP/1.", 0) != 0 || text.size() < 12) {
				return Parsed{ .status = -1 };
			}
			result.status = std::atoi(text.c_str() + 9);
			continue;
		}
		const auto colon = text.find(':');
		if (colon == std::string::npos || colon == 0) {
			continue;
		}
		auto name = Lower(text.substr(0, colon));
		auto value = text.substr(colon + 1);
		while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
			value.erase(value.begin());
		}
		while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
			value.pop_back();
		}
		if (name == "set-cookie" || name == "set-cookie2") {
			continue;
		} else if (name == "location") {
			result.location = value;
		} else if (name == "transfer-encoding") {
			result.chunked = (Lower(value).find("chunked") != std::string::npos);
		} else if (name == "content-length") {
			char *tail = nullptr;
			const auto parsed = std::strtoull(value.c_str(), &tail, 10);
			if (tail == value.c_str() || *tail != 0) {
				return Parsed{ .status = -1 };
			}
			result.length = size_t(parsed);
		}
		result.headers.emplace_back(std::move(name), std::move(value));
	}
	return result;
}

[[nodiscard]] bool DecodeChunked(
		const std::string &raw,
		size_t maxBytes,
		std::string &out,
		bool &complete) {
	auto at = size_t(0);
	complete = false;
	out.clear();
	while (true) {
		const auto eol = raw.find("\r\n", at);
		if (eol == std::string::npos) {
			return true;
		}
		char *tail = nullptr;
		const auto size = std::strtoull(raw.c_str() + at, &tail, 16);
		if (tail == raw.c_str() + at) {
			return false;
		}
		at = eol + 2;
		if (size == 0) {
			complete = true;
			return true;
		}
		if (out.size() + size > maxBytes) {
			return false;
		}
		if (raw.size() < at + size + 2) {
			return true;
		}
		out.append(raw, at, size);
		at += size + 2;
	}
}

[[nodiscard]] int Remaining(const QDeadlineTimer &deadline) {
	return int(std::max<qint64>(1, deadline.remainingTime()));
}

[[nodiscard]] int Slice(const QDeadlineTimer &deadline) {
	return std::min(Remaining(deadline), kWaitSliceMs);
}

[[nodiscard]] fgpl::FetchError Aborted() {
	return Error("closed", u"the connection was closed"_q);
}

[[nodiscard]] fgpl::Result<QHostInfo, fgpl::FetchError> Lookup(
		const QString &host,
		const Connection &entry,
		const QDeadlineTimer &deadline) {
	struct State {
		std::mutex mutex;
		std::condition_variable done;
		std::optional<QHostInfo> info;
	};
	const auto state = std::make_shared<State>();
	crl::async([=] {
		auto info = QHostInfo::fromName(host);
		{
			const auto lock = std::scoped_lock(state->mutex);
			state->info = std::move(info);
		}
		state->done.notify_all();
	});
	auto lock = std::unique_lock(state->mutex);
	while (!state->info) {
		if (entry.killed.load()) {
			return Aborted();
		} else if (deadline.hasExpired()) {
			return Error("timeout", u"the host lookup timed out"_q);
		}
		state->done.wait_for(lock, std::chrono::milliseconds(Slice(deadline)));
	}
	return *state->info;
}

[[nodiscard]] fgpl::Result<QHostAddress, fgpl::FetchError> Resolve(
		const std::string &host,
		bool ip,
		const Connection &entry,
		const QDeadlineTimer &deadline,
		bool local = false) {
	if (local) {
		if (ip) {
			const auto address = QHostAddress(QString::fromStdString(host));
			if (address.isNull() || !address.isLoopback()) {
				return Error("blocked", u"the address is not on this computer"_q);
			}
			return address;
		} else if (host == "localhost") {
			return QHostAddress(QHostAddress::LocalHost);
		}
		return Error("blocked", u"only localhost is allowed"_q);
	}
	if (ip) {
		const auto address = QHostAddress(QString::fromStdString(host));
		if (address.isNull()) {
			return Error("invalid", u"a bad address"_q);
		} else if (!fgpl::net::IsPublicAddress(address.toString().toStdString())) {
			return Error("blocked", u"the address is not public"_q);
		}
		return address;
	}
	const auto lookup = Lookup(QString::fromStdString(host), entry, deadline);
	if (!lookup.ok()) {
		return lookup.error();
	}
	const auto &info = lookup.value();
	if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
		return Error("dns", u"the host was not found"_q);
	}
	for (const auto &address : info.addresses()) {
		if (!fgpl::net::IsPublicAddress(address.toString().toStdString())) {
			return Error("blocked", u"the address is not public"_q);
		}
	}
	return info.addresses().front();
}

[[nodiscard]] fgpl::Result<qintptr, fgpl::FetchError> Connect(
		const QHostAddress &address,
		int port,
		const Connection &entry,
		const QDeadlineTimer &deadline) {
	auto storage = sockaddr_storage();
	auto length = socklen_t(0);
	auto valid = false;
	if (const auto v4 = address.toIPv4Address(&valid); valid) {
		auto &in = reinterpret_cast<sockaddr_in&>(storage);
		in.sin_family = AF_INET;
		in.sin_port = htons(quint16(port));
		in.sin_addr.s_addr = htonl(v4);
		length = sizeof(sockaddr_in);
	} else if (address.protocol() == QAbstractSocket::IPv6Protocol) {
		auto &in6 = reinterpret_cast<sockaddr_in6&>(storage);
		in6.sin6_family = AF_INET6;
		in6.sin6_port = htons(quint16(port));
		const auto raw = address.toIPv6Address();
		std::memcpy(&in6.sin6_addr, &raw, sizeof(raw));
		length = sizeof(sockaddr_in6);
	} else {
		return Error("invalid", u"a bad address"_q);
	}
	const auto fd = ::socket(storage.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return Error("connect", QString::fromLocal8Bit(std::strerror(errno)));
	}
	const auto fail = [&](fgpl::FetchError error) {
		::close(fd);
		return error;
	};
	if (::connect(fd, reinterpret_cast<const sockaddr*>(&storage), length) != 0
		&& errno != EINPROGRESS) {
		return fail(Error("connect", QString::fromLocal8Bit(std::strerror(errno))));
	}
	while (true) {
		if (entry.killed.load()) {
			return fail(Aborted());
		} else if (deadline.hasExpired()) {
			return fail(Error("connect", u"the connection timed out"_q));
		}
		auto item = pollfd{ fd, POLLOUT, 0 };
		const auto ready = ::poll(&item, 1, Slice(deadline));
		if (ready < 0 && errno != EINTR) {
			return fail(Error("connect", QString::fromLocal8Bit(std::strerror(errno))));
		} else if (ready <= 0) {
			continue;
		}
		auto error = 0;
		auto size = socklen_t(sizeof(error));
		if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0) {
			error = errno;
		}
		if (error) {
			return fail(Error("connect", QString::fromLocal8Bit(std::strerror(error))));
		}
		return qintptr(fd);
	}
}

[[nodiscard]] std::optional<fgpl::FetchError> AttachDescriptor(
		QTcpSocket &socket,
		qintptr descriptor) {
	socket.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
	if (!socket.setSocketDescriptor(descriptor, QAbstractSocket::ConnectedState)) {
		::close(int(descriptor));
		return Error("connect", socket.errorString());
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<fgpl::FetchError> ProxyError(QString text) {
	return Error("connect", u"proxy: "_q + text);
}

[[nodiscard]] std::optional<fgpl::FetchError> SendAll(
		int fd,
		const QByteArray &data,
		const Connection &entry,
		const QDeadlineTimer &deadline) {
	auto sent = qsizetype(0);
	while (sent < data.size()) {
		if (entry.killed.load()) {
			return Aborted();
		} else if (deadline.hasExpired()) {
			return ProxyError(u"the handshake timed out"_q);
		}
		auto item = pollfd{ fd, POLLOUT, 0 };
		const auto ready = ::poll(&item, 1, Slice(deadline));
		if (ready < 0 && errno != EINTR) {
			return ProxyError(QString::fromLocal8Bit(std::strerror(errno)));
		} else if (ready <= 0) {
			continue;
		}
		const auto written = ::send(
			fd,
			data.constData() + sent,
			size_t(data.size() - sent),
			MSG_NOSIGNAL);
		if (written < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				continue;
			}
			return ProxyError(QString::fromLocal8Bit(std::strerror(errno)));
		}
		sent += qsizetype(written);
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<fgpl::FetchError> RecvExact(
		int fd,
		char *out,
		size_t size,
		const Connection &entry,
		const QDeadlineTimer &deadline) {
	auto got = size_t(0);
	while (got < size) {
		if (entry.killed.load()) {
			return Aborted();
		} else if (deadline.hasExpired()) {
			return ProxyError(u"the handshake timed out"_q);
		}
		auto item = pollfd{ fd, POLLIN, 0 };
		const auto ready = ::poll(&item, 1, Slice(deadline));
		if (ready < 0 && errno != EINTR) {
			return ProxyError(QString::fromLocal8Bit(std::strerror(errno)));
		} else if (ready <= 0) {
			continue;
		}
		const auto read = ::recv(fd, out + got, size - got, 0);
		if (read == 0) {
			return ProxyError(u"the connection was closed"_q);
		} else if (read < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				continue;
			}
			return ProxyError(QString::fromLocal8Bit(std::strerror(errno)));
		}
		got += size_t(read);
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<fgpl::FetchError> Socks5Negotiate(
		int fd,
		const NetworkProxy &proxy,
		const Connection &entry,
		const QDeadlineTimer &deadline) {
	const auto authorize = !proxy.user.empty();
	if (proxy.user.size() > 255 || proxy.password.size() > 255) {
		return ProxyError(u"the credentials are too long"_q);
	}
	auto hello = QByteArray();
	hello.append(char(5));
	if (authorize) {
		hello.append(char(2));
		hello.append(char(0));
		hello.append(char(2));
	} else {
		hello.append(char(1));
		hello.append(char(0));
	}
	if (const auto error = SendAll(fd, hello, entry, deadline)) {
		return error;
	}
	char reply[2] = {};
	if (const auto error = RecvExact(fd, reply, 2, entry, deadline)) {
		return error;
	} else if (reply[0] != 5) {
		return ProxyError(u"not a SOCKS5 server"_q);
	} else if (reply[1] == char(0xFF)) {
		return ProxyError(u"no acceptable authentication method"_q);
	} else if (reply[1] == 0) {
		return std::nullopt;
	} else if (reply[1] != 2 || !authorize) {
		return ProxyError(u"an unsupported authentication method"_q);
	}
	auto credentials = QByteArray();
	credentials.append(char(1));
	credentials.append(char(proxy.user.size()));
	credentials.append(proxy.user.data(), qsizetype(proxy.user.size()));
	credentials.append(char(proxy.password.size()));
	credentials.append(proxy.password.data(), qsizetype(proxy.password.size()));
	if (const auto error = SendAll(fd, credentials, entry, deadline)) {
		return error;
	} else if (const auto error = RecvExact(fd, reply, 2, entry, deadline)) {
		return error;
	} else if (reply[1] != 0) {
		return ProxyError(u"the credentials were rejected"_q);
	}
	return std::nullopt;
}

[[nodiscard]] fgpl::Result<QByteArray, fgpl::FetchError> Socks5Address(
		const std::string &host,
		bool ip,
		int port) {
	auto result = QByteArray();
	const auto address = ip
		? QHostAddress(QString::fromStdString(host))
		: QHostAddress();
	if (ip && address.isNull()) {
		return Error("invalid", u"a bad address"_q);
	} else if (ip && address.protocol() == QAbstractSocket::IPv6Protocol) {
		result.append(char(4));
		const auto raw = address.toIPv6Address();
		result.append(reinterpret_cast<const char*>(&raw), sizeof(raw));
	} else if (ip) {
		result.append(char(1));
		const auto raw = htonl(address.toIPv4Address());
		result.append(reinterpret_cast<const char*>(&raw), sizeof(raw));
	} else if (host.empty() || host.size() > 255) {
		return Error("invalid", u"a bad host name"_q);
	} else {
		result.append(char(3));
		result.append(char(host.size()));
		result.append(host.data(), qsizetype(host.size()));
	}
	result.append(char((port >> 8) & 0xFF));
	result.append(char(port & 0xFF));
	return result;
}

[[nodiscard]] QString Socks5ReplyText(int code) {
	switch (code) {
	case 1: return u"a general failure"_q;
	case 2: return u"the connection is not allowed by the rules"_q;
	case 3: return u"the network is unreachable"_q;
	case 4: return u"the host is unreachable"_q;
	case 5: return u"the connection was refused"_q;
	case 6: return u"the TTL expired"_q;
	case 7: return u"the command is not supported"_q;
	case 8: return u"the address type is not supported"_q;
	}
	return u"an unknown error"_q;
}

[[nodiscard]] std::optional<fgpl::FetchError> Socks5Reply(
		int fd,
		QHostAddress &bound,
		int &boundPort,
		const Connection &entry,
		const QDeadlineTimer &deadline) {
	char head[4] = {};
	if (const auto error = RecvExact(fd, head, 4, entry, deadline)) {
		return error;
	} else if (head[0] != 5) {
		return ProxyError(u"not a SOCKS5 server"_q);
	} else if (head[1] != 0) {
		return ProxyError(Socks5ReplyText(int(uint8_t(head[1]))));
	}
	bound = QHostAddress();
	auto skip = size_t(0);
	switch (head[3]) {
	case 1: {
		char raw[4] = {};
		if (const auto error = RecvExact(fd, raw, 4, entry, deadline)) {
			return error;
		}
		auto value = uint32_t();
		std::memcpy(&value, raw, sizeof(value));
		bound = QHostAddress(quint32(ntohl(value)));
	} break;
	case 4: {
		char raw[16] = {};
		if (const auto error = RecvExact(fd, raw, 16, entry, deadline)) {
			return error;
		}
		auto value = Q_IPV6ADDR();
		std::memcpy(&value, raw, sizeof(raw));
		bound = QHostAddress(value);
	} break;
	case 3: {
		char length = 0;
		if (const auto error = RecvExact(fd, &length, 1, entry, deadline)) {
			return error;
		}
		skip = size_t(uint8_t(length));
	} break;
	default:
		return ProxyError(u"a malformed reply"_q);
	}
	if (skip) {
		auto name = std::string(skip, '\0');
		if (const auto error = RecvExact(fd, name.data(), skip, entry, deadline)) {
			return error;
		}
	}
	char port[2] = {};
	if (const auto error = RecvExact(fd, port, 2, entry, deadline)) {
		return error;
	}
	boundPort = (int(uint8_t(port[0])) << 8) | int(uint8_t(port[1]));
	return std::nullopt;
}

[[nodiscard]] std::optional<fgpl::FetchError> HttpConnect(
		int fd,
		const NetworkProxy &proxy,
		const std::string &host,
		bool ip,
		int port,
		const Connection &entry,
		const QDeadlineTimer &deadline) {
	const auto name = (ip && host.find(':') != std::string::npos)
		? "[" + host + "]"
		: host;
	const auto target = name + ":" + std::to_string(port);
	auto request = QByteArray::fromStdString(
		"CONNECT " + target + " HTTP/1.1\r\nHost: " + target + "\r\n");
	if (!proxy.user.empty()) {
		request.append("Proxy-Authorization: Basic ");
		request.append(QByteArray::fromStdString(
			proxy.user + ":" + proxy.password).toBase64());
		request.append("\r\n");
	}
	request.append("\r\n");
	if (const auto error = SendAll(fd, request, entry, deadline)) {
		return error;
	}
	auto head = std::string();
	while (head.size() < 4 || head.compare(head.size() - 4, 4, "\r\n\r\n") != 0) {
		if (head.size() > kMaxHeaderBytes) {
			return ProxyError(u"the reply is too large"_q);
		}
		char byte = 0;
		if (const auto error = RecvExact(fd, &byte, 1, entry, deadline)) {
			return error;
		}
		head.push_back(byte);
	}
	if (head.rfind("HTTP/1.", 0) != 0 || head.size() < 12) {
		return ProxyError(u"not an HTTP proxy"_q);
	}
	const auto status = std::atoi(head.c_str() + 9);
	if (status == 407) {
		return ProxyError(u"authentication is required"_q);
	} else if (status < 200 || status > 299) {
		return ProxyError(u"refused with status "_q + QString::number(status));
	}
	return std::nullopt;
}

struct ProxySocket {
	qintptr descriptor = -1;
	QHostAddress address;
};

[[nodiscard]] fgpl::Result<ProxySocket, fgpl::FetchError> ConnectProxy(
		const NetworkProxy &proxy,
		const Connection &entry,
		const QDeadlineTimer &deadline) {
	const auto text = QString::fromStdString(proxy.host);
	auto address = QHostAddress(text);
	if (address.isNull()) {
		const auto lookup = Lookup(text, entry, deadline);
		if (!lookup.ok()) {
			return lookup.error();
		}
		const auto &info = lookup.value();
		if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
			return Error("dns", u"the proxy host was not found"_q);
		}
		address = info.addresses().front();
	}
	const auto connected = Connect(address, proxy.port, entry, deadline);
	if (!connected.ok()) {
		return connected.error();
	}
	return ProxySocket{ connected.value(), address };
}

[[nodiscard]] fgpl::Result<qintptr, fgpl::FetchError> ConnectThrough(
		const NetworkProxy &proxy,
		const std::string &host,
		bool ip,
		int port,
		Connection &entry,
		const QDeadlineTimer &deadline) {
	const auto opened = ConnectProxy(proxy, entry, deadline);
	if (!opened.ok()) {
		return opened.error();
	}
	const auto fd = int(opened.value().descriptor);
	entry.address = opened.value().address;
	const auto fail = [&](fgpl::FetchError error) {
		::close(fd);
		return error;
	};
	if (proxy.type == NetworkProxy::Type::Http) {
		if (const auto error = HttpConnect(fd, proxy, host, ip, port, entry, deadline)) {
			return fail(*error);
		}
		return qintptr(fd);
	}
	if (const auto error = Socks5Negotiate(fd, proxy, entry, deadline)) {
		return fail(*error);
	}
	const auto address = Socks5Address(host, ip, port);
	if (!address.ok()) {
		return fail(address.error());
	}
	auto request = QByteArray("\x05\x01\x00", 3);
	request.append(address.value());
	auto bound = QHostAddress();
	auto boundPort = 0;
	if (const auto error = SendAll(fd, request, entry, deadline)) {
		return fail(*error);
	} else if (const auto error = Socks5Reply(fd, bound, boundPort, entry, deadline)) {
		return fail(*error);
	}
	return qintptr(fd);
}

[[nodiscard]] fgpl::Result<qintptr, fgpl::FetchError> OpenStream(
		const NetworkProxy &proxy,
		const std::string &host,
		bool ip,
		int port,
		Connection &entry,
		const QDeadlineTimer &deadline,
		bool local = false) {
	if (!local && proxy.type != NetworkProxy::Type::None) {
		entry.via = ProxyLabel(proxy);
		return ConnectThrough(proxy, host, ip, port, entry, deadline);
	}
	const auto resolved = Resolve(host, ip, entry, deadline, local);
	if (!resolved.ok()) {
		return resolved.error();
	}
	entry.address = resolved.value();
	return Connect(resolved.value(), port, entry, deadline);
}

struct UdpRelay {
	QHostAddress address;
	int port = 0;
	std::unique_ptr<QTcpSocket> control;
};

[[nodiscard]] fgpl::Result<UdpRelay, fgpl::FetchError> Socks5Associate(
		const NetworkProxy &proxy,
		Connection &entry,
		const QDeadlineTimer &deadline) {
	const auto opened = ConnectProxy(proxy, entry, deadline);
	if (!opened.ok()) {
		return opened.error();
	}
	const auto fd = int(opened.value().descriptor);
	const auto fail = [&](fgpl::FetchError error) {
		::close(fd);
		return error;
	};
	if (const auto error = Socks5Negotiate(fd, proxy, entry, deadline)) {
		return fail(*error);
	}
	const auto request = QByteArray("\x05\x03\x00\x01\x00\x00\x00\x00\x00\x00", 10);
	auto bound = QHostAddress();
	auto boundPort = 0;
	if (const auto error = SendAll(fd, request, entry, deadline)) {
		return fail(*error);
	} else if (const auto error = Socks5Reply(fd, bound, boundPort, entry, deadline)) {
		return fail(*error);
	}
	if (bound.isNull()
		|| bound == QHostAddress::AnyIPv4
		|| bound == QHostAddress::AnyIPv6
		|| bound == QHostAddress::Any) {
		bound = opened.value().address;
	}
	if (boundPort <= 0) {
		return fail(Error("connect", u"proxy: no relay port"_q));
	}
	auto result = UdpRelay();
	result.address = bound;
	result.port = boundPort;
	result.control = std::make_unique<QTcpSocket>();
	if (const auto error = AttachDescriptor(*result.control, qintptr(fd))) {
		return *error;
	}
	return result;
}

[[nodiscard]] qsizetype UnwrapDatagram(const QByteArray &data) {
	if (data.size() < 4 || data[0] != 0 || data[1] != 0 || data[2] != 0) {
		return -1;
	}
	const auto type = int(uint8_t(data[3]));
	const auto address = (type == 1)
		? qsizetype(4)
		: (type == 4)
		? qsizetype(16)
		: (type == 3 && data.size() > 4)
		? qsizetype(1) + qsizetype(uint8_t(data[4]))
		: qsizetype(-1);
	if (address < 0) {
		return -1;
	}
	const auto offset = qsizetype(4) + address + qsizetype(2);
	return (data.size() >= offset) ? offset : qsizetype(-1);
}

[[nodiscard]] std::optional<fgpl::FetchError> Encrypt(
		QSslSocket &socket,
		const Connection &entry,
		const QDeadlineTimer &deadline) {
	socket.startClientEncryption();
	while (!socket.isEncrypted()) {
		if (entry.killed.load()) {
			return Aborted();
		} else if (deadline.hasExpired()) {
			return Error("tls", u"the handshake timed out"_q);
		} else if (!socket.waitForEncrypted(Slice(deadline))
			&& socket.state() != QAbstractSocket::ConnectedState) {
			return Error("tls", socket.errorString());
		}
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<fgpl::FetchError> Flush(
		QTcpSocket &socket,
		const Connection &entry,
		const QDeadlineTimer &deadline) {
	while (socket.bytesToWrite() > 0) {
		if (entry.killed.load()) {
			return Aborted();
		} else if (deadline.hasExpired()) {
			return Error("timeout", u"the data was not sent in time"_q);
		} else if (!socket.waitForBytesWritten(Slice(deadline))
			&& socket.state() != QAbstractSocket::ConnectedState) {
			return Error("closed", socket.errorString());
		}
	}
	return std::nullopt;
}

class NetTransport final : public fgpl::Transport {
public:
	fgpl::Result<fgpl::FetchResponse, fgpl::FetchError> fetch(
			const fgpl::FetchRequest &request) override {
		const auto entry = Register(
			request.owner,
			u"HTTPS"_q,
			QString::fromStdString(request.host),
			request.port);
		auto result = fetchHops(request, entry);
		auto event = EventOf(*entry);
		event.action = QString::fromStdString(request.method);
		const auto url = QUrl(QString::fromStdString(request.url));
		event.target = url.host() + url.path().left(kMaxLogPath);
		if (result.ok()) {
			event.result = QString::number(result.value().status);
			event.failed = (result.value().status >= 400);
		} else {
			event.result = QString::fromStdString(result.error().code + ": " + result.error().message);
			event.failed = true;
		}
		Unregister(entry->id);
		AddLog(std::move(event));
		return result;
	}

	fgpl::Result<int64_t, fgpl::FetchError> open(
			const fgpl::NetOwner &owner,
			const fgpl::SocketRequest &request) override {
		auto result = openSocket(owner, request);
		auto event = NetworkEvent();
		event.plugin = QString::fromStdString(owner.plugin);
		event.kind = (request.kind == fgpl::SocketKind::Udp)
			? u"UDP"_q
			: request.tls
			? u"TLS"_q
			: u"TCP"_q;
		event.action = u"open"_q;
		event.target = QString::fromStdString(request.host) + ':' + QString::number(request.port);
		if (!result.ok()) {
			event.result = QString::fromStdString(result.error().code + ": " + result.error().message);
			event.failed = true;
		} else if (const auto entry = Find(result.value())) {
			event.result = entry->via.isEmpty()
				? entry->address.toString()
				: u"via "_q + entry->via;
		}
		AddLog(std::move(event));
		return result;
	}

private:
	fgpl::Result<fgpl::FetchResponse, fgpl::FetchError> fetchHops(
			const fgpl::FetchRequest &request,
			const std::shared_ptr<Connection> &entry) {
		if (CurrentPolicy().blockHttps) {
			return Error("blocked", u"HTTPS is blocked for plugins"_q);
		}
		if (request.local) {
			const auto parsed = fgpl::net::ParseLocalUrl(request.url);
			if (!parsed.ok()
				|| (parsed.value().scheme != "http"
					&& parsed.value().scheme != "https")) {
				return Error("invalid", u"url"_q);
			}
			entry->host = QString::fromStdString(parsed.value().host);
			entry->port = parsed.value().port;
			auto response = once(
				*entry,
				parsed.value(),
				request.method,
				request.headers,
				request.body,
				request.maxResponseBytes,
				QDeadlineTimer(request.timeoutMs),
				true);
			if (!response.ok()) {
				return response.error();
			}
			return std::move(response.value().first);
		}
		auto url = request.url;
		auto method = request.method;
		auto body = request.body;
		auto headers = request.headers;
		auto host = request.host;
		auto previousHost = host;
		const auto deadline = QDeadlineTimer(request.timeoutMs);
		for (auto hop = 0; hop <= kMaxRedirects; ++hop) {
			const auto parsed = fgpl::net::ParseUrl(url);
			if (!parsed.ok()) {
				return Error("invalid", u"url"_q);
			}
			entry->host = QString::fromStdString(parsed.value().host);
			entry->port = parsed.value().port;
			auto response = once(
				*entry,
				parsed.value(),
				method,
				headers,
				body,
				request.maxResponseBytes,
				deadline);
			if (!response.ok()) {
				return response.error();
			}
			const auto &value = response.value();
			const auto redirect = (value.first.status == 301
				|| value.first.status == 302
				|| value.first.status == 303
				|| value.first.status == 307
				|| value.first.status == 308);
			if (!redirect || value.second.empty()) {
				return std::move(response.value().first);
			}
			const auto next = QUrl(QString::fromStdString(url))
				.resolved(QUrl(QString::fromStdString(value.second)));
			const auto target = next.toString(QUrl::FullyEncoded).toStdString();
			if (next.scheme() != u"https"_q
				|| !request.allowRedirect
				|| !request.allowRedirect(target)) {
				return Error("redirect", u"a redirect is not allowed"_q);
			}
			const auto nextParsed = fgpl::net::ParseUrl(target);
			if (!nextParsed.ok()) {
				return Error("redirect", u"a redirect is not allowed"_q);
			}
			if (nextParsed.value().host != parsed.value().host) {
				std::erase_if(headers, [](const auto &entry) {
					return Lower(entry.first) == "authorization";
				});
			}
			if (value.first.status == 303
				|| ((value.first.status == 301 || value.first.status == 302)
					&& method == "POST")) {
				method = "GET";
				body.clear();
			}
			url = nextParsed.value().text;
		}
		return Error("redirect", u"too many redirects"_q);
	}

	fgpl::Result<int64_t, fgpl::FetchError> openSocket(
			const fgpl::NetOwner &owner,
			const fgpl::SocketRequest &request) {
		const auto udp = (request.kind == fgpl::SocketKind::Udp);
		const auto host = QString::fromStdString(request.host);
		const auto deadline = QDeadlineTimer(request.timeoutMs);
		auto entry = Register(
			owner,
			udp ? u"UDP"_q : request.tls ? u"TLS"_q : u"TCP"_q,
			host,
			request.port);
		const auto failWith = [&](fgpl::FetchError error) {
			Unregister(entry->id);
			return error;
		};
		const auto fail = [&](const char *code, QString message) {
			return failWith(Error(code, message));
		};
		const auto policy = CurrentPolicy();
		if (BlockedKind(policy, entry->kind)) {
			return fail("blocked", entry->kind + u" is blocked for plugins"_q);
		}
		const auto proxied = !request.local
			&& (policy.proxy.type != NetworkProxy::Type::None);
		entry->tls = request.tls;
		if (udp) {
			auto socket = std::make_unique<QUdpSocket>();
			socket->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
			auto local = QAbstractSocket::NetworkLayerProtocol();
			if (proxied) {
				if (policy.proxy.type == NetworkProxy::Type::Http) {
					return fail("unavailable", u"UDP is not supported through an HTTP proxy"_q);
				}
				entry->via = ProxyLabel(policy.proxy);
				auto relay = Socks5Associate(policy.proxy, *entry, deadline);
				if (!relay.ok()) {
					return failWith(relay.error());
				}
				const auto header = Socks5Address(request.host, request.ip, request.port);
				if (!header.ok()) {
					return failWith(header.error());
				}
				entry->control = std::move(relay.value().control);
				entry->relay = relay.value().address;
				entry->relayPort = relay.value().port;
				entry->datagramHeader = QByteArray("\x00\x00\x00", 3) + header.value();
				local = entry->relay.protocol();
			} else {
				const auto resolved = Resolve(
					request.host,
					request.ip,
					*entry,
					deadline,
					request.local);
				if (!resolved.ok()) {
					return failWith(resolved.error());
				}
				entry->address = resolved.value();
				local = resolved.value().protocol();
			}
			const auto any = (local == QAbstractSocket::IPv6Protocol)
				? QHostAddress::AnyIPv6
				: QHostAddress::AnyIPv4;
			if (!socket->bind(QHostAddress(any), 0)) {
				return fail("connect", socket->errorString());
			}
			entry->datagram = std::move(socket);
			return entry->id;
		}
		auto socket = std::unique_ptr<QTcpSocket>();
		if (request.tls) {
			auto secure = std::make_unique<QSslSocket>();
			auto configuration = QSslConfiguration::defaultConfiguration();
			configuration.setProtocol(QSsl::TlsV1_2OrLater);
			secure->setSslConfiguration(configuration);
			secure->setPeerVerifyMode(QSslSocket::VerifyPeer);
			secure->setPeerVerifyName(host);
			socket = std::move(secure);
		} else {
			socket = std::make_unique<QTcpSocket>();
		}
		const auto opened = OpenStream(
			policy.proxy,
			request.host,
			request.ip,
			request.port,
			*entry,
			deadline,
			request.local);
		if (!opened.ok()) {
			return failWith(opened.error());
		} else if (const auto error = AttachDescriptor(*socket, opened.value())) {
			return failWith(*error);
		}
		if (const auto secure = dynamic_cast<QSslSocket*>(socket.get())) {
			if (const auto error = Encrypt(*secure, *entry, deadline)) {
				return failWith(*error);
			}
		}
		entry->stream = std::move(socket);
		return entry->id;
	}

public:
	fgpl::Result<size_t, fgpl::FetchError> write(
			int64_t id,
			std::span<const uint8_t> data,
			int timeoutMs) override {
		const auto entry = Find(id);
		if (!entry || entry->killed.load()) {
			return Error("closed", u"the connection is closed"_q);
		}
		const auto bytes = QByteArray(
			reinterpret_cast<const char*>(data.data()),
			qsizetype(data.size()));
		if (BlockedKind(CurrentPolicy(), entry->kind)) {
			return Error("blocked", entry->kind + u" is blocked for plugins"_q);
		}
		if (entry->datagram) {
			if (entry->control
				&& entry->control->state() != QAbstractSocket::ConnectedState) {
				return Error("closed", u"the proxy closed the connection"_q);
			}
			const auto now = crl::now();
			auto &recent = entry->recentDatagrams;
			while (!recent.empty() && recent.front() <= now - 1000) {
				recent.pop_front();
			}
			if (int(recent.size()) >= kDatagramsPerSecond) {
				return Error("limit", u"too many datagrams"_q);
			}
			recent.push_back(now);
			const auto proxied = !entry->datagramHeader.isEmpty();
			const auto written = entry->datagram->writeDatagram(
				proxied ? entry->datagramHeader + bytes : bytes,
				proxied ? entry->relay : entry->address,
				quint16(proxied ? entry->relayPort : entry->port));
			if (written < 0) {
				return Error("write", entry->datagram->errorString());
			}
			const auto payload = size_t(written)
				- (proxied ? size_t(entry->datagramHeader.size()) : size_t(0));
			entry->sent += uint64(payload);
			return payload;
		}
		const auto socket = entry->stream.get();
		if (socket->state() != QAbstractSocket::ConnectedState) {
			return Error("closed", u"the connection is closed"_q);
		}
		const auto deadline = QDeadlineTimer(timeoutMs);
		if (socket->write(bytes) != bytes.size()) {
			return Error("write", socket->errorString());
		}
		if (const auto error = Flush(*socket, *entry, deadline)) {
			return *error;
		}
		entry->sent += uint64(bytes.size());
		return data.size();
	}

	fgpl::Result<fgpl::SocketRead, fgpl::FetchError> read(
			int64_t id,
			size_t maxBytes,
			int timeoutMs) override {
		const auto entry = Find(id);
		if (!entry || entry->killed.load()) {
			return Error("closed", u"the connection is closed"_q);
		}
		const auto deadline = QDeadlineTimer(timeoutMs);
		auto result = fgpl::SocketRead();
		if (const auto socket = entry->datagram.get()) {
			while (true) {
				while (socket->hasPendingDatagrams()) {
					auto sender = QHostAddress();
					auto port = quint16();
					auto buffer = QByteArray(
						int(std::max<qint64>(socket->pendingDatagramSize(), 0)),
						Qt::Uninitialized);
					const auto got = socket->readDatagram(
						buffer.data(),
						buffer.size(),
						&sender,
						&port);
					const auto proxied = !entry->datagramHeader.isEmpty();
					if (got < 0
						|| port != (proxied ? entry->relayPort : entry->port)
						|| !sender.isEqual(proxied ? entry->relay : entry->address)) {
						continue;
					}
					auto offset = qsizetype(0);
					if (proxied) {
						offset = UnwrapDatagram(buffer.left(qsizetype(got)));
						if (offset < 0) {
							continue;
						}
					}
					const auto payload = size_t(got) - size_t(offset);
					entry->received += uint64(payload);
					const auto size = std::min<size_t>(payload, maxBytes);
					result.data.assign(
						buffer.constData() + offset,
						buffer.constData() + offset + size);
					return result;
				}
				if (entry->killed.load()) {
					return Error("closed", u"the connection was closed by the user"_q);
				} else if (deadline.hasExpired()) {
					return result;
				}
				socket->waitForReadyRead(Slice(deadline));
			}
		}
		const auto socket = entry->stream.get();
		while (!socket->bytesAvailable()
			&& socket->state() == QAbstractSocket::ConnectedState
			&& !deadline.hasExpired()) {
			if (entry->killed.load()) {
				return Error("closed", u"the connection was closed by the user"_q);
			}
			socket->waitForReadyRead(Slice(deadline));
		}
		const auto chunk = socket->read(qint64(maxBytes));
		entry->received += uint64(chunk.size());
		result.data.assign(chunk.constData(), chunk.constData() + chunk.size());
		result.closed = (socket->state() != QAbstractSocket::ConnectedState)
			&& !socket->bytesAvailable();
		return result;
	}

	void close(int64_t id) override {
		Unregister(id);
	}

	void abort(const fgpl::NetOwner &owner) override {
		auto &registry = GetRegistry();
		const auto lock = std::scoped_lock(registry.mutex);
		for (const auto &[id, connection] : registry.connections) {
			if (connection->owner.plugin == owner.plugin
				&& connection->owner.account == owner.account) {
				connection->killed.store(true);
			}
		}
	}

	fgpl::NetTraffic traffic(const std::string &plugin) override {
		auto &registry = GetRegistry();
		const auto lock = std::scoped_lock(registry.mutex);
		auto result = fgpl::NetTraffic();
		if (const auto i = registry.finished.find(plugin); i != registry.finished.end()) {
			result.received = i->second.received;
			result.sent = i->second.sent;
		}
		for (const auto &[id, connection] : registry.connections) {
			if (connection->owner.plugin == plugin) {
				result.received += connection->received.load();
				result.sent += connection->sent.load();
				result.open += (connection->kind != u"HTTPS"_q) ? 1 : 0;
			}
		}
		return result;
	}

private:
	using Single = fgpl::Result<
		std::pair<fgpl::FetchResponse, std::string>,
		fgpl::FetchError>;

	[[nodiscard]] Single once(
			Connection &entry,
			const fgpl::net::Url &url,
			const std::string &method,
			const std::vector<std::pair<std::string, std::string>> &headers,
			const fgpl::cbor::Bytes &body,
			size_t maxBytes,
			const QDeadlineTimer &deadline,
			bool local = false) {
		const auto host = QString::fromStdString(url.host);
		const auto opened = OpenStream(
			CurrentPolicy().proxy,
			url.host,
			url.ip,
			url.port,
			entry,
			deadline,
			local);
		if (!opened.ok()) {
			return opened.error();
		}
		auto socket = QSslSocket();
		auto configuration = QSslConfiguration::defaultConfiguration();
		configuration.setProtocol(QSsl::TlsV1_2OrLater);
		socket.setSslConfiguration(configuration);
		socket.setPeerVerifyMode(QSslSocket::VerifyPeer);
		socket.setPeerVerifyName(host);
		if (const auto error = AttachDescriptor(socket, opened.value())) {
			return *error;
		} else if (url.tls) {
			if (const auto encrypted = Encrypt(socket, entry, deadline)) {
				return *encrypted;
			}
		}

		auto head = method + " " + url.target + " HTTP/1.1\r\nHost: "
			+ ((url.host.find(':') != std::string::npos)
				? "[" + url.host + "]"
				: url.host);
		if (url.port != (url.tls ? 443 : 80)) {
			head += ":" + std::to_string(url.port);
		}
		head += "\r\nConnection: close\r\nAccept-Encoding: identity\r\n";
		auto hasAgent = false;
		for (const auto &[name, value] : headers) {
			hasAgent = hasAgent || (Lower(name) == "user-agent");
			head += name + ": " + value + "\r\n";
		}
		if (!hasAgent) {
			head += "User-Agent: freshGram-plugin\r\n";
		}
		if (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH") {
			head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
		}
		head += "\r\n";
		socket.write(head.data(), qint64(head.size()));
		if (!body.empty()) {
			socket.write(reinterpret_cast<const char*>(body.data()), qint64(body.size()));
		}
		entry.sent += head.size() + body.size();
		if (const auto error = Flush(socket, entry, deadline)) {
			return *error;
		}

		auto raw = std::string();
		auto parsed = std::optional<Parsed>();
		auto payload = std::string();
		auto done = false;
		while (!done) {
			if (entry.killed.load()) {
				return Error("closed", u"the connection was closed by the user"_q);
			}
			if (!socket.bytesAvailable()
				&& socket.state() == QAbstractSocket::ConnectedState
				&& !socket.waitForReadyRead(Slice(deadline))
				&& socket.state() == QAbstractSocket::ConnectedState) {
				if (deadline.hasExpired()) {
					return Error("timeout", u"the request timed out"_q);
				}
				continue;
			}
			const auto chunk = socket.readAll();
			raw.append(chunk.constData(), size_t(chunk.size()));
			entry.received += uint64(chunk.size());
			const auto closed = (socket.state() != QAbstractSocket::ConnectedState
				&& !socket.bytesAvailable());
			if (!parsed) {
				if (raw.size() > kMaxHeaderBytes && raw.find("\r\n\r\n") == std::string::npos) {
					return Error("response", u"the headers are too large"_q);
				}
				parsed = ParseHead(raw);
				if (parsed && parsed->status < 0) {
					return Error("response", u"a malformed response"_q);
				}
			}
			if (parsed) {
				const auto available = raw.size() - parsed->consumed;
				if (parsed->chunked) {
					auto complete = false;
					if (!DecodeChunked(raw.substr(parsed->consumed), maxBytes, payload, complete)) {
						return Error("response", u"a malformed or too large body"_q);
					}
					done = complete;
				} else if (parsed->length) {
					if (*parsed->length > maxBytes) {
						return Error("limit", u"the response is too large"_q);
					}
					done = (available >= *parsed->length);
					if (done) {
						payload = raw.substr(parsed->consumed, *parsed->length);
					}
				} else {
					if (available > maxBytes) {
						return Error("limit", u"the response is too large"_q);
					}
					if (closed) {
						payload = raw.substr(parsed->consumed);
						done = true;
					}
				}
			}
			if (!done && closed) {
				return Error("response", u"the connection closed early"_q);
			}
		}
		socket.abort();

		auto response = fgpl::FetchResponse();
		response.status = parsed->status;
		response.headers = parsed->headers;
		response.body.assign(payload.begin(), payload.end());
		return std::make_pair(std::move(response), parsed->location);
	}

};

} // namespace

void SetNetworkPolicy(NetworkPolicy policy) {
	{
		auto &state = GetPolicyState();
		const auto lock = std::scoped_lock(state.mutex);
		state.policy = policy;
	}
	auto &registry = GetRegistry();
	const auto lock = std::scoped_lock(registry.mutex);
	for (const auto &[id, connection] : registry.connections) {
		if (BlockedKind(policy, connection->kind)) {
			connection->killed.store(true);
		}
	}
}

fgpl::Transport *NetworkTransport() {
	static auto result = NetTransport();
	return &result;
}

std::vector<NetworkEvent> NetworkLog() {
	auto &registry = GetRegistry();
	const auto lock = std::scoped_lock(registry.mutex);
	return { registry.log.begin(), registry.log.end() };
}

uint64 NetworkLogVersion() {
	return GetRegistry().logVersion.load();
}

void ClearNetworkLog() {
	auto &registry = GetRegistry();
	const auto lock = std::scoped_lock(registry.mutex);
	registry.log.clear();
	++registry.logVersion;
}

std::vector<ConnectionView> Connections() {
	auto &registry = GetRegistry();
	const auto lock = std::scoped_lock(registry.mutex);
	auto result = std::vector<ConnectionView>();
	for (const auto &[id, connection] : registry.connections) {
		if (connection->killed.load()) {
			continue;
		}
		result.push_back({
			.id = id,
			.plugin = QString::fromStdString(connection->owner.plugin),
			.account = QString::fromStdString(connection->owner.account),
			.kind = connection->kind,
			.host = connection->host,
			.port = connection->port,
			.address = connection->address.toString(),
			.via = connection->via,
			.tls = connection->tls,
			.received = connection->received.load(),
			.sent = connection->sent.load(),
			.opened = connection->opened,
		});
	}
	return result;
}

void CloseConnection(int64 id) {
	if (const auto connection = Find(id)) {
		connection->killed.store(true);
	}
}

#else

void SetNetworkPolicy(NetworkPolicy) {
}

fgpl::Transport *NetworkTransport() {
	return nullptr;
}

std::vector<NetworkEvent> NetworkLog() {
	return {};
}

uint64 NetworkLogVersion() {
	return 0;
}

void ClearNetworkLog() {
}

std::vector<ConnectionView> Connections() {
	return {};
}

void CloseConnection(int64 id) {
}

#endif

} // namespace FreshPlugins
