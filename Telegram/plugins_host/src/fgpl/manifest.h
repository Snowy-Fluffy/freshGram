#pragma once

#include "fgpl/cbor.h"
#include "fgpl/result.h"
#include "fgpl/semver.h"
#include "fgpl/signature.h"

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
};

struct MenuItem {
	std::string surface;
	std::string id;
	Localized title;
	std::string icon;
};

struct Command {
	std::string name;
	Localized description;
};

struct OutgoingFilter {
	std::set<int64_t> chats;
	std::set<std::string> chatTypes;
};

struct OutgoingHook {
	bool declared = false;
	bool block = false;
	OutgoingFilter filter;
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
};

// The permission an event type needs, or null for an unknown event.
[[nodiscard]] const char *EventPermission(const std::string &event);
// Events of a chat follow the plugin's chat scope and never come from secret chats.
[[nodiscard]] bool EventIsChatScoped(const std::string &event);

[[nodiscard]] const std::set<std::string> &OutgoingChatTypes();
[[nodiscard]] bool Matches(
	const OutgoingFilter &filter,
	const OutgoingInfo &info);

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

	std::set<std::string> activation = { "event", "command", "menu", "settings", "message", "timer" };
	bool background = false;

	std::vector<PermissionRequest> permissions;
	std::vector<std::string> events;
	std::vector<MenuItem> menus;
	std::map<std::string, std::string> icons;
	std::vector<Command> commands;
	bool settingsPage = false;
	OutgoingHook outgoing;
	std::vector<std::string> messagesFrom;

	std::vector<Dependency> dependencies;
	std::vector<Dependency> conflicts;
	int storageSchema = 1;
	std::string updatesFeed;

	std::map<std::string, std::string> files;
	std::optional<ManifestSignature> signature;
};

[[nodiscard]] const std::set<std::string> &HostFeatures();

struct HostEnvironment {
	Version app;
	int apiLevel = 4;
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

} // namespace fgpl
