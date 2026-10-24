#pragma once

#include "ayu/secret/secret_crypto.h"

#include <QtCore/QString>

#include <string>

namespace FreshSecret::Vault {

[[nodiscard]] bool Available();

[[nodiscard]] bool IsSealed(const Bytes &data);
[[nodiscard]] bool IsBound(const Bytes &data);
[[nodiscard]] Bytes Seal(const Bytes &plain, const std::string &context);
[[nodiscard]] bool Open(
	const Bytes &sealed,
	Bytes &plain,
	const std::string &context);

[[nodiscard]] bool SealToFile(
	const QString &path,
	const Bytes &plain,
	const std::string &context);
[[nodiscard]] bool OpenFromFile(
	const QString &path,
	Bytes &plain,
	const std::string &context,
	bool *legacy = nullptr);

} // namespace FreshSecret::Vault
