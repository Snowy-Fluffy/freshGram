// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

namespace AyuMapper {

template<typename MTPObject>
[[nodiscard]] MTPObject deserializeObject(std::vector<char> serialized);

template<typename MTPObject>
[[nodiscard]] std::vector<char> serializeObject(MTPObject object);

[[nodiscard]] std::vector<char> serializeReactions(not_null<HistoryItem*> item);
[[nodiscard]] MTPMessageReactions deserializeReactions(const std::vector<char> &serialized);
std::pair<std::string, std::vector<char>> serializeTextWithEntities(not_null<HistoryItem*> item);
[[nodiscard]] MTPVector<MTPMessageEntity> deserializeTextWithEntities(std::vector<char> serialized);
[[nodiscard]] std::vector<char> serializeSavableMedia(const MTPMessageMedia &media);
[[nodiscard]] MTPMessageMedia deserializeMedia(const std::vector<char> &serialized);
int mapItemFlagsToMTPFlags(not_null<HistoryItem*> item);

} // namespace AyuMapper
