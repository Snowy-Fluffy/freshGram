#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace FreshDatabaseBackup {

[[nodiscard]] bool create();
[[nodiscard]] bool restore(int &index);
void startPeriodic();
void removeAll();
void purgeUser(long long userId);
void configure(int keepCount, int intervalHours);

// The richest copy outlives the rotation only while the files are kept.
void setKeepRichest(bool keep);

// The statements run on every copy a few seconds later, put together.
void purgeLater(const QStringList &statements);
[[nodiscard]] QStringList OfMessage(
	long long userId,
	long long dialogId,
	long long messageId);
[[nodiscard]] QStringList OfDialog(
	long long userId,
	long long dialogId,
	long long topicId);
[[nodiscard]] QStringList OlderThan(int cutoff);

} // namespace FreshDatabaseBackup
