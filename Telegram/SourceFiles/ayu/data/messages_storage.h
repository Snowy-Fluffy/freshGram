// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "ayu/data/entities.h"

class History;

namespace AyuMessages {

constexpr auto kServiceDocumentType = 2;

void addEditedMessage(not_null<HistoryItem *> item);
bool isBotMessage(not_null<const HistoryItem*> item);
ID storageUserId(not_null<PeerData*> peer);
std::vector<AyuMessageBase> loadEditedMessages(ID userId, ID dialogId, ID messageId, ID minId, ID maxId, int totalLimit);
bool hasRevisions(not_null<const HistoryItem*> item);

void addDeletedMessage(not_null<HistoryItem*> item);
void cacheDeletedMedia(not_null<HistoryItem*> item);
void restoreSavedMedia(not_null<HistoryItem*> item, const AyuMessageBase &message);
std::vector<ID> loadDeletedDialogIds(ID userId);
void saveTtlMedia(not_null<HistoryItem*> item);
std::optional<MTPMessageMedia> savedTtlMedia(not_null<History*> history, MsgId id);
void restoreTtlBytes(not_null<HistoryItem*> item);
MTPMessageMedia restoredMedia(const AyuMessageBase &message);
std::vector<AyuMessageBase> searchDeletedMessages(ID userId, ID dialogId, ID topicId, ID fromId, const std::string &query, int limit);
std::vector<DeletedExtra> loadDeletedExtras(ID userId, ID dialogId);
void restoreExtra(not_null<HistoryItem*> item, const DeletedExtra &extra);
std::vector<AyuMessageBase> loadDeletedMessages(ID userId, ID dialogId, ID topicId, ID minId, ID maxId, int totalLimit, const std::string &searchQuery);
bool hasDeletedMessages(not_null<PeerData*> peer, ID topicId);
void flushPending();
void removeDeletedMessage(not_null<HistoryItem*> item);
void clearDeletedMessages(not_null<PeerData*> peer, ID topicId);

}
