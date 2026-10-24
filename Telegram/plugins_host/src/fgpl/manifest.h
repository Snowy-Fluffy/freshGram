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
};

struct Command {
	std::string name;
	Localized description;
};

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
	int apiMax = 1;
	std::set<std::string> platforms = { "linux", "windows", "macos" };
	std::map<std::string, VersionRange> platformVersions;
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

	std::set<std::string> activation = { "event", "command", "menu", "settings" };
	bool background = false;

	std::vector<PermissionRequest> permissions;
	std::vector<std::string> events;
	std::vector<MenuItem> menus;
	std::vector<Command> commands;
	bool settingsPage = false;
	std::string outgoing;

	std::vector<Dependency> dependencies;
	std::vector<Dependency> conflicts;
	int storageSchema = 1;
	std::string updatesFeed;

	std::map<std::string, std::string> files;
	std::optional<ManifestSignature> signature;
};

struct HostEnvironment {
	Version app;
	int apiLevel = 1;
	std::string platform;
	Version platformVersion;
	std::set<std::string> features;
	bool premium = false;
};

enum class Problem : uint8_t {
	AppTooOld,
	AppTooNew,
	ApiUnsupported,
	PlatformUnsupported,
	PlatformVersion,
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
