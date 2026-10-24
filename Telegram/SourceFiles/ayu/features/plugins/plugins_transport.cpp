// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_transport.h"

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/broker.h"
#include "fgpl/net_policy.h"
#endif

#include <QtCore/QDeadlineTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QHostInfo>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslSocket>
#include <QtNetwork/QUdpSocket>

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

namespace FreshPlugins {

#ifdef FRESHGRAM_PLUGINS

namespace {

constexpr auto kMaxRedirects = 5;
constexpr auto kMaxHeaderBytes = size_t(64) << 10;
constexpr auto kWaitSliceMs = 50;
constexpr auto kDatagramsPerSecond = 200;

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
};

[[nodiscard]] Registry &GetRegistry() {
	static auto result = Registry();
	return result;
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

[[nodiscard]] fgpl::Result<QHostAddress, fgpl::FetchError> Resolve(
		const std::string &host,
		bool ip) {
	if (ip) {
		const auto address = QHostAddress(QString::fromStdString(host));
		if (address.isNull()) {
			return Error("invalid", u"a bad address"_q);
		}
		return address;
	}
	const auto lookup = QHostInfo::fromName(QString::fromStdString(host));
	if (lookup.error() != QHostInfo::NoError || lookup.addresses().isEmpty()) {
		return Error("dns", u"the host was not found"_q);
	}
	for (const auto &address : lookup.addresses()) {
		if (!fgpl::net::IsPublicAddress(address.toString().toStdString())) {
			return Error("blocked", u"the address is not public"_q);
		}
	}
	return lookup.addresses().front();
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
		const auto guard = gsl::finally([&] { Unregister(entry->id); });
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

	fgpl::Result<int64_t, fgpl::FetchError> open(
			const fgpl::NetOwner &owner,
			const fgpl::SocketRequest &request) override {
		const auto udp = (request.kind == fgpl::SocketKind::Udp);
		const auto resolved = Resolve(request.host, request.ip);
		if (!resolved.ok()) {
			return resolved.error();
		}
		const auto host = QString::fromStdString(request.host);
		auto entry = Register(
			owner,
			udp ? u"UDP"_q : request.tls ? u"TLS"_q : u"TCP"_q,
			host,
			request.port);
		entry->address = resolved.value();
		entry->tls = request.tls;
		const auto fail = [&](const char *code, QString message) {
			Unregister(entry->id);
			return Error(code, message);
		};
		if (udp) {
			auto socket = std::make_unique<QUdpSocket>();
			socket->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
			const auto any = (resolved.value().protocol() == QAbstractSocket::IPv6Protocol)
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
		socket->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
		socket->connectToHost(resolved.value(), quint16(request.port));
		if (!socket->waitForConnected(request.timeoutMs)) {
			return fail("connect", socket->errorString());
		}
		if (const auto secure = dynamic_cast<QSslSocket*>(socket.get())) {
			secure->startClientEncryption();
			if (!secure->waitForEncrypted(request.timeoutMs)) {
				return fail("tls", secure->errorString());
			}
		}
		entry->stream = std::move(socket);
		return entry->id;
	}

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
		if (entry->datagram) {
			const auto now = crl::now();
			auto &recent = entry->recentDatagrams;
			while (!recent.empty() && recent.front() <= now - 1000) {
				recent.pop_front();
			}
			if (int(recent.size()) >= kDatagramsPerSecond) {
				return Error("limit", u"too many datagrams"_q);
			}
			recent.push_back(now);
			const auto written = entry->datagram->writeDatagram(
				bytes,
				entry->address,
				quint16(entry->port));
			if (written < 0) {
				return Error("write", entry->datagram->errorString());
			}
			entry->sent += uint64(written);
			return size_t(written);
		}
		const auto socket = entry->stream.get();
		if (socket->state() != QAbstractSocket::ConnectedState) {
			return Error("closed", u"the connection is closed"_q);
		}
		const auto deadline = QDeadlineTimer(timeoutMs);
		if (socket->write(bytes) != bytes.size()) {
			return Error("write", socket->errorString());
		}
		while (socket->bytesToWrite() > 0) {
			if (entry->killed.load() || deadline.hasExpired()) {
				return Error("timeout", u"the data was not sent in time"_q);
			} else if (!socket->waitForBytesWritten(std::min(remaining(deadline), kWaitSliceMs))
				&& socket->state() != QAbstractSocket::ConnectedState) {
				return Error("closed", socket->errorString());
			}
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
					if (got < 0 || port != entry->port || !sender.isEqual(entry->address)) {
						continue;
					}
					entry->received += uint64(got);
					const auto size = std::min<size_t>(size_t(got), maxBytes);
					result.data.assign(buffer.constData(), buffer.constData() + size);
					return result;
				}
				if (entry->killed.load()) {
					return Error("closed", u"the connection was closed by the user"_q);
				} else if (deadline.hasExpired()) {
					return result;
				}
				socket->waitForReadyRead(std::min(remaining(deadline), kWaitSliceMs));
			}
		}
		const auto socket = entry->stream.get();
		while (!socket->bytesAvailable()
			&& socket->state() == QAbstractSocket::ConnectedState
			&& !deadline.hasExpired()) {
			if (entry->killed.load()) {
				return Error("closed", u"the connection was closed by the user"_q);
			}
			socket->waitForReadyRead(std::min(remaining(deadline), kWaitSliceMs));
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
			const QDeadlineTimer &deadline) {
		const auto host = QString::fromStdString(url.host);
		const auto resolved = Resolve(url.host, url.ip);
		if (!resolved.ok()) {
			return resolved.error();
		}
		entry.address = resolved.value();
		auto socket = QSslSocket();
		auto configuration = QSslConfiguration::defaultConfiguration();
		configuration.setProtocol(QSsl::TlsV1_2OrLater);
		socket.setSslConfiguration(configuration);
		socket.setPeerVerifyMode(QSslSocket::VerifyPeer);
		socket.setPeerVerifyName(host);
		socket.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
		socket.connectToHost(resolved.value(), quint16(url.port));
		if (!socket.waitForConnected(remaining(deadline))) {
			return Error("connect", socket.errorString());
		}
		socket.startClientEncryption();
		if (!socket.waitForEncrypted(remaining(deadline))) {
			return Error("tls", socket.errorString());
		}

		auto head = method + " " + url.target + " HTTP/1.1\r\nHost: " + url.host;
		if (url.port != 443) {
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
		if (!socket.waitForBytesWritten(remaining(deadline))) {
			return Error("write", socket.errorString());
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
				&& !socket.waitForReadyRead(std::min(remaining(deadline), kWaitSliceMs))
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

	[[nodiscard]] static int remaining(const QDeadlineTimer &deadline) {
		return int(std::max<qint64>(1, deadline.remainingTime()));
	}

};

} // namespace

fgpl::Transport *NetworkTransport() {
	static auto result = NetTransport();
	return &result;
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

fgpl::Transport *NetworkTransport() {
	return nullptr;
}

std::vector<ConnectionView> Connections() {
	return {};
}

void CloseConnection(int64 id) {
}

#endif

} // namespace FreshPlugins
