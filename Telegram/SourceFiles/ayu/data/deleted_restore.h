#pragma once

#include "ayu/data/entities.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "base/weak_ptr.h"
#include "data/data_msg_id.h"

class History;
class HistoryItem;

namespace AyuRestore {

struct Row {
	AyuMessageBase message;
	MsgId localId;
	bool dead = false;
	std::optional<DeletedExtra> extra;
};

void noteDeleted(not_null<History*> history);

// Calls back with the ids of dialogs that have saved deleted messages.
void withDeletedDialogs(
	ID userId,
	Fn<void(const base::flat_set<ID> &)> callback);

class State final : public base::has_weak_ptr {
public:
	explicit State(not_null<History*> history);

	void checkLoaded();
	void disable();
	void markStale();
	HistoryItem *materialize(TimeId from, TimeId till);
	void dropDuplicates();
	[[nodiscard]] HistoryItem *find(MsgId id, not_null<HistoryItem*> holder);
	void restoreThread(MsgId rootId);
	[[nodiscard]] HistoryItem *itemFor(const AyuMessageBase &message);
	[[nodiscard]] std::vector<not_null<HistoryItem*>> orphans(
		TimeId from,
		TimeId till) const;

private:
	void load(ID userId, ID dialogId);
	void resolveWaiting();
	void materializeThread(MsgId rootId);
	HistoryItem *create(Row &row);
	[[nodiscard]] HistoryItem *duplicateOf(const Row &row) const;

	const not_null<History*> _history;
	std::vector<Row> _rows;
	base::flat_map<ID, int> _index;
	base::flat_set<FullMsgId> _waiting;
	base::flat_set<MsgId> _threads;
	bool _requested = false;
	bool _loaded = false;
	bool _pending = false;
	bool _materializing = false;
	bool _disabled = false;
	bool _stale = false;

};

} // namespace AyuRestore
