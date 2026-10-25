#pragma once

#include "fgpl/broker.h"
#include "fgpl/manifest.h"

#include <set>
#include <string>
#include <vector>

namespace fgpl {

struct GrantDecision {
	std::set<std::string> granted;
	std::optional<std::set<int64_t>> chats;
};

enum class Risk : uint8_t {
	ExfiltrationPossible,
	AnyServer,
	RawSockets,
	DeletedMessages,
	SendsInYourName,
	ChangesOutgoing,
	ChangesMessages,
	Background,
	Clipboard,
	Files,
	PluginMessages,
};

enum class Danger : uint8_t {
	Low,
	Medium,
	High,
};

[[nodiscard]] Danger PermissionDanger(
	const std::string &id,
	const std::string &scope = {});

struct RiskNote {
	Risk risk = Risk::ExfiltrationPossible;
	std::string text;
};

[[nodiscard]] Grants MakeGrants(
	const Manifest &manifest,
	const GrantDecision &decision);

[[nodiscard]] std::vector<RiskNote> RiskNotes(
	const std::set<std::string> &granted);

[[nodiscard]] std::vector<std::string> NewRequests(
	const Manifest &previous,
	const Manifest &next);

} // namespace fgpl
