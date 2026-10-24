#pragma once

#include "fgpl/cbor.h"
#include "fgpl/result.h"
#include "fgpl/vfs.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fgpl {

struct RuntimeLimits {
	uint64_t memoryBytes = uint64_t(64) << 20;
	uint32_t callTimeoutMs = 500;
	uint32_t startTimeoutMs = 10000;
	size_t maxModuleBytes = size_t(32) << 20;
	size_t maxMessageBytes = size_t(1) << 20;
	size_t maxCallsPerDispatch = 256;
	uint32_t maxHostCallMs = 60000;
	cbor::Limits cbor;
};

struct RuntimeOptions {
	size_t stackBytes = size_t(512) << 10;
	uint64_t memoryReservation = 0;
	uint64_t memoryGuard = 0;
	bool baselineCompiler = false;
};

enum class LoadError : uint8_t {
	TooLarge,
	InvalidModule,
	ForbiddenImport,
	MissingExport,
	BadExport,
	Instantiate,
	Init,
};

struct LoadFailure {
	LoadError error = LoadError::InvalidModule;
	std::string detail;
};

enum class CallError : uint8_t {
	Dead,
	BadMessage,
	Timeout,
	Trap,
	GuestFailed,
};

namespace guest_status {

constexpr auto kOk = int32_t(0);
constexpr auto kBadArguments = int32_t(-1);
constexpr auto kBadRequest = int32_t(-2);
constexpr auto kBadResponse = int32_t(-3);
constexpr auto kNoMemory = int32_t(-4);
constexpr auto kTooManyCalls = int32_t(-5);

} // namespace guest_status

struct ProcessSpec {
	std::vector<std::string> args;
	std::vector<std::string> env;
};

struct Dispatch {
	enum class Kind : uint8_t {
		Init,
		Event,
		Stop,
	};

	Kind kind = Kind::Stop;
	int64_t id = 0;
	cbor::Value value;
};

class Dispatcher {
public:
	virtual ~Dispatcher() = default;

	[[nodiscard]] virtual Dispatch next() = 0;
	virtual void finished(int64_t id, int32_t status) = 0;
};

class Bridge {
public:
	virtual ~Bridge() = default;

	[[nodiscard]] virtual cbor::Value call(const cbor::Value &request) = 0;
	virtual void log(std::string_view line) = 0;
};

struct RuntimeState;
struct PluginState;

class Plugin {
public:
	~Plugin();

	[[nodiscard]] Result<int32_t, CallError> init(const cbor::Value &info);
	[[nodiscard]] Result<int32_t, CallError> deliver(const cbor::Value &event);
	[[nodiscard]] Result<int32_t, CallError> runCommand(Dispatcher &dispatcher);

	[[nodiscard]] bool isCommand() const;

	[[nodiscard]] bool dead() const;
	[[nodiscard]] const std::string &lastError() const;
	[[nodiscard]] uint64_t memoryBytes() const;

private:
	friend class Runtime;

	explicit Plugin(std::unique_ptr<PluginState> state);

	std::unique_ptr<PluginState> _state;

};

class Runtime {
public:
	explicit Runtime(const RuntimeOptions &options = {});
	~Runtime();

	[[nodiscard]] static const char *EngineVersion();

	[[nodiscard]] Result<std::unique_ptr<Plugin>, LoadFailure> load(
		std::span<const uint8_t> wasm,
		Bridge &bridge,
		const RuntimeLimits &limits = {},
		std::shared_ptr<const Vfs> files = nullptr,
		const ProcessSpec &process = {});

	[[nodiscard]] Result<std::unique_ptr<Plugin>, LoadFailure> loadCompiled(
		std::span<const uint8_t> compiled,
		Bridge &bridge,
		const RuntimeLimits &limits = {},
		std::shared_ptr<const Vfs> files = nullptr,
		const ProcessSpec &process = {});

	[[nodiscard]] Result<std::vector<uint8_t>, LoadFailure> compile(
		std::span<const uint8_t> wasm,
		const RuntimeLimits &limits = {});

private:
	[[nodiscard]] Result<std::unique_ptr<Plugin>, LoadFailure> loadFrom(
		std::span<const uint8_t> bytes,
		bool precompiled,
		Bridge &bridge,
		const RuntimeLimits &limits,
		std::shared_ptr<const Vfs> files,
		const ProcessSpec &process);

	std::shared_ptr<RuntimeState> _state;

};

} // namespace fgpl
