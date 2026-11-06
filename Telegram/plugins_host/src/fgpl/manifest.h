#pragma once

#include "fgpl/cbor.h"
#include "fgpl/result.h"
#include "fgpl/semver.h"
#include "fgpl/signature.h"
#include "fgpl/text_entity.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fgpl {

struct Localized {
	std::map<std::string, std::string> text;

	[[nodiscard]] std::string get(const std::string &language) const;
};

struct PermissionRequest {
	std::string id;
	Localized reason;
	std::string scope;
	bool required = false;
	// Ports of this computer that net.local may reach, as inclusive ranges.
	std::vector<std::pair<int, int>> ports;
	// "read" or "write" for fs.folder.
	std::string mode;
};

// Declarative visibility of a menu item.
struct MenuWhen {
	std::set<std::string> chatTypes;
	std::optional<bool> out;
	std::optional<bool> hasMedia;
	std::optional<bool> hasText;
	std::optional<bool> forwarded;
	std::set<std::string> media;

	[[nodiscard]] bool empty() const {
		return chatTypes.empty()
			&& !out
			&& !hasMedia
			&& !hasText
			&& !forwarded
			&& media.empty();
	}
};

// What the app knows about the chat and the message a menu opens for.
struct MenuContext {
	std::string chatType;
	bool out = false;
	std::string media;
	bool hasText = false;
	bool forwarded = false;
};

struct MenuItem {
	std::string surface;
	std::string id;
	Localized title;
	std::string icon;
	MenuWhen when;
	int priority = 0;
};

[[nodiscard]] const std::set<std::string> &MenuSurfaces();
// Surfaces that open for messages and may use the message keys of `when`.
[[nodiscard]] bool MenuSurfaceHasMessages(const std::string &surface);
[[nodiscard]] const std::set<std::string> &MenuMediaKinds();
[[nodiscard]] bool Matches(const MenuWhen &when, const MenuContext &context);

struct Command {
	std::string name;
	Localized description;
};

struct DeepLink {
	std::string path;
	Localized description;
};

struct StorageExport {
	bool declared = false;
	std::vector<std::string> prefixes;
	std::vector<std::string> exclude;

	[[nodiscard]] bool includes(const std::string &key) const;
};

struct TranslatorDecl {
	bool declared = false;
	std::string name;
	std::vector<std::string> languages;
	int maxChars = 4096;
};

struct AiProviderDecl {
	bool declared = false;
	std::string name;
	int maxChars = 4096;
};

struct TranscriberDecl {
	bool declared = false;
	std::string name;
	int maxSeconds = 300;
};

struct OutgoingFilter {
	std::set<int64_t> chats;
	std::set<std::string> chatTypes;
};

struct OutgoingHook {
	bool declared = false;
	bool block = false;
	// Also sees the sends of files, where the text is the caption.
	bool media = false;
	int priority = 0;
	// Once this hook has changed a send, only the later guards with onFailure block see it.
	bool final = false;
	OutgoingFilter filter;
};

struct IncomingHook {
	bool declared = false;
	bool block = false;
	bool hold = false;
	// Also sees service messages (a pin, a joined member), which it can only hide.
	bool service = false;
	int priority = 0;
	OutgoingFilter filter;
};

struct EventFilter {
	OutgoingFilter where;
	MenuWhen when;
};

// What the app knows about an incoming message that is about to be shown:
// `message` holds its fields except text and entities, which change as plugins answer.
struct IncomingInfo {
	int64_t chat = 0;
	bool secret = false;
	std::string chatType;
	bool edited = false;
	bool service = false;
	std::string text;
	std::vector<TextEntity> entities;
	cbor::Map message;
};

// A file of a send: kind is photo, video, audio or file.
struct OutgoingFile {
	std::string kind;
	std::string name;
	int64_t size = 0;
};

// What the app knows about a message that is about to be sent.
struct OutgoingInfo {
	int64_t chat = 0;
	bool secret = false;
	std::string chatType;
	int64_t topic = 0;
	int64_t replyTo = 0;
	bool silent = false;
	int64_t scheduled = 0;
	std::string text;
	std::vector<TextEntity> entities;
	// Not empty for a send of files.
	std::vector<OutgoingFile> files;
	uint64_t send = 0;
};

// The permission an event type needs, or null for an unknown event.
[[nodiscard]] const char *EventPermission(const std::string &event);
[[nodiscard]] const std::vector<std::string> &KnownEvents();
// Events of a chat follow the plugin's chat scope and never come from secret chats.
[[nodiscard]] bool EventIsChatScoped(const std::string &event);
[[nodiscard]] bool MatchesEvent(const EventFilter &filter, const cbor::Value &event);

[[nodiscard]] const std::set<std::string> &OutgoingChatTypes();
[[nodiscard]] bool Matches(
	const OutgoingFilter &filter,
	const OutgoingInfo &info);
[[nodiscard]] bool Matches(
	const OutgoingFilter &filter,
	const IncomingInfo &info);

struct Dependency {
	std::string id;
	std::optional<VersionRange> range;
};

struct ManifestSignature {
	PublicKey key = {};
	SignatureBytes value = {};
};

struct Manifest {
	int schema = 1;
	std::string id;
	Localized name;
	Version version;
	Localized description;
	std::string authorName;
	std::string authorUrl;
	std::string authorContact;
	std::string license;
	std::string homepage;
	std::string repository;
	std::string support;
	std::string changelog;
	std::string icon;
	std::vector<std::string> categories;
	std::vector<std::string> keywords;
	std::vector<std::string> locales;

	VersionRange appRange = *VersionRange::Parse("*");
	std::optional<Version> testedWith;
	int apiMin = 1;
	int apiMax = 1000;
	std::vector<std::string> needs;
	bool premiumOnly = false;

	std::string kind;
	std::string module;
	std::string entry;
	std::string pythonVersion;
	std::string language;
	int memoryMb = 64;
	int eventTimeoutMs = 500;
	int storageMb = 10;

	std::set<std::string> activation = { "event", "command", "menu", "settings", "message", "timer", "translate", "ai", "toast", "link", "transcribe" };
	bool background = false;

	std::vector<PermissionRequest> permissions;
	std::vector<std::string> events;
	std::map<std::string, EventFilter> eventFilters;
	std::vector<MenuItem> menus;
	std::map<std::string, std::string> icons;
	std::vector<Command> commands;
	std::vector<DeepLink> links;
	bool settingsPage = false;
	OutgoingHook outgoing;
	IncomingHook incoming;
	TranslatorDecl translator;
	AiProviderDecl aiProvider;
	TranscriberDecl transcriber;
	std::vector<std::string> messagesFrom;

	std::vector<Dependency> dependencies;
	std::vector<Dependency> conflicts;
	int storageSchema = 1;
	StorageExport storageExport;
	std::string updatesFeed;

	std::map<std::string, std::string> files;
	std::optional<ManifestSignature> signature;
};

[[nodiscard]] const std::set<std::string> &HostFeatures();

struct HostEnvironment {
	Version app;
	int apiLevel = 9;
	std::set<std::string> features = HostFeatures();
	bool premium = false;
};

enum class Problem : uint8_t {
	AppTooOld,
	AppTooNew,
	ApiUnsupported,
	MissingFeature,
	PremiumRequired,
};

struct CompatIssue {
	Problem problem = Problem::AppTooOld;
	std::string detail;
};

[[nodiscard]] Result<Manifest, std::vector<std::string>> ParseManifest(
	const cbor::Value &document);

[[nodiscard]] std::vector<CompatIssue> CheckCompatibility(
	const Manifest &manifest,
	const HostEnvironment &environment);

[[nodiscard]] bool Outdated(
	const Manifest &manifest,
	const HostEnvironment &environment);

[[nodiscard]] const std::set<std::string> &KnownPermissions();

[[nodiscard]] std::string SignedText(const cbor::Value &document);

enum class ReservedName : uint8_t {
	None,
	Id,
	Author,
};

[[nodiscard]] ReservedName ClaimedReservedName(const Manifest &manifest);

} // namespace fgpl
