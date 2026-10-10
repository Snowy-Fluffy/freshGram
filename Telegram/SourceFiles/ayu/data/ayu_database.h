// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "ayu/data/entities.h"

#include <functional>
#include <optional>

class SchemaVersion
{
public:
	int id;
	int version;
};

namespace AyuDatabase {

void initialize();

void addEditedMessage(const EditedMessage &message, int maxRevisions);
void purgeOlderThan(int days);
std::vector<EditedMessage> getEditedMessages(ID userId, ID dialogId, ID messageId, ID minId, ID maxId, int totalLimit);
bool hasRevisions(ID userId, ID dialogId, ID messageId);

void addDeletedMessage(const DeletedMessage &message);
void addDeletedMessages(const std::vector<DeletedMessage> &messages);
void addDeletedExtras(const std::vector<DeletedExtra> &extras);
std::vector<DeletedExtra> getDeletedExtras(ID userId, ID dialogId);
std::vector<DeletedMessage> searchDeletedMessages(ID userId, ID dialogId, ID topicId, ID fromId, const std::string &searchQuery, int totalLimit);
std::vector<DeletedMessage> getDeletedMessages(ID userId, ID dialogId, ID topicId, ID minId, ID maxId, int totalLimit, const std::string &searchQuery = "");
bool hasDeletedMessages(ID userId, ID dialogId, ID topicId);
std::vector<ID> getDeletedDialogIds(ID userId);
void removeDeletedMessage(ID userId, ID dialogId, ID messageId);
void clearDeletedMessages(ID userId, ID dialogId, ID topicId);
void clearAllDeleted();

void saveSecretChat(const SecretChatRow &chat);
std::vector<SecretChatRow> getSecretChats(ID userId);
void removeSecretChat(ID userId, int chatId);
bool addSecretMessage(const SecretMessageRow &message);
void updateSecretMessage(const SecretMessageRow &message);
std::vector<SecretMessageRow> getSecretMessages(ID userId, int chatId);
void removeSecretMessage(ID userId, int chatId, ID randomId);
void clearSecretMessages(ID userId, int chatId);

void savePeekedStatus(const PeekedStatusRow &row);
std::vector<PeekedStatusRow> getPeekedStatus(ID userId, ID targetId);
void saveSecretState(const SecretStateRow &row);
std::vector<SecretStateRow> getSecretState(ID userId);
void savePeekRestore(const PeekRestoreRow &row);
std::vector<PeekRestoreRow> getPeekRestore(ID userId);
void clearPeekRestore(ID userId);

void saveKeptDialog(const KeptDialog &dialog);
void syncKeptDialogs(const std::vector<KeptDialog> &dialogs);
void saveKeptTopics(const std::vector<KeptTopic> &topics);
std::vector<KeptTopic> getKeptTopics(ID userId, ID dialogId);
std::vector<KeptTopic> getKeptTopicsFor(ID userId);
void removeKeptTopics(ID userId, ID dialogId);
void removeKeptTopic(ID userId, ID dialogId, ID rootId);
std::vector<KeptDialog> getKeptDialogs(ID userId);
void removeKeptDialog(ID userId, ID dialogId);

void saveKnownUsers(const std::vector<KnownUser> &users);
std::optional<KnownUser> getKnownUser(ID userId, ID peerId);

std::vector<RegexFilter> getAllRegexFilters();
RegexFilter getById(std::vector<char> id);
std::vector<RegexFilter> getShared();
std::vector<RegexFilter> getByDialogId(ID dialogId);
std::vector<RegexFilterGlobalExclusion> getAllFiltersExclusions();
std::vector<RegexFilter> getExcludedByDialogId(ID dialogId);

int getCount();


void addRegexFilter(const RegexFilter &filter);
void addRegexExclusion(const RegexFilterGlobalExclusion &exclusion);

void updateRegexFilter(const RegexFilter &filter);

void deleteFilter(const std::vector<char> &id);
void deleteExclusionsByFilterId(const std::vector<char> &id);
void deleteExclusion(ID dialogId, std::vector<char> filterId);

void deleteAllFilters();
void deleteAllExclusions();

bool hasFilters();
bool hasPerDialogFilters();

void moveCurrentDatabase();
void backupCurrentDatabase();

}
