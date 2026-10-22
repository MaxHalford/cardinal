//===----------------------------------------------------------------------===//
// Models just in time.
//
//   in memory, fresh?        use it
//   stored, fresh?           read once, keep in memory
//   otherwise                fit, keep in memory, save
//
// A model covers the columns queries filter on, and a fanout column per known
// foreign key: few columns keep the tree on what estimates need. e.g. with
// every column of title, the fanouts of movie_companies and movie_keyword
// hang off episode_of_id, and their correlation is lost.
//
// Fresh: the table's row count is within MAX_DRIFT of the model's, and the
// model covers the columns and fanouts known now.
//
// By default the query that needs a model waits for its fit. With
// cardinal_background, fits run on a worker thread instead: the query plans
// with what there is, the old model or DuckDB's own estimates, and later
// queries get the new model. Off by default: the worker's transactions block
// CHECKPOINT, and keep a closed database open until the fit ends.
//
// Models fitted here are saved in the store (store.hpp), with the keys learned
// so far: never in the user's database.
//===----------------------------------------------------------------------===//

#include "models.hpp"
#include "fit.hpp"
#include "inner_query.hpp"
#include "keys.hpp"
#include "model_store.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/object_cache.hpp"

#include <condition_variable>
#include <thread>

namespace duckdb {
namespace cardinal {

//! Share of the row count a table may grow or shrink by before its model is refitted.
static constexpr double MAX_DRIFT = 0.1;

//! A table's model in DuckDB's object cache, which lives as long as the database.
//! Fitting holds the lock: concurrent planners wait instead of fitting twice.
struct CachedModel : public ObjectCacheEntry {
	static string ObjectType() {
		return "cardinal_model";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	//! Not evictable: models are small, and building one runs queries.
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx();
	}

	mutex lock;
	shared_ptr<Model> model;
	//! The stored model was read: it is not read again.
	bool read = false;
	//! A background fit is queued or running.
	bool queued = false;
	//! Fitting failed for this row count, columns and fanouts: not tried again until they change.
	optional_idx failed_rows;
	idx_t failed_fanouts = 0;
};

//! e.g. "cardinal:imdb.main.title"
static string Key(TableCatalogEntry &table) {
	return "cardinal:" + table.ParentCatalog().GetName().GetIdentifierName() + "." +
	       table.ParentSchema().name.GetIdentifierName() + "." + table.name.GetIdentifierName();
}

//! The table's rows now. Invalid for tables DuckDB does not store.
static optional_idx CurrentRows(TableCatalogEntry &table) {
	if (!table.IsDuckTable()) {
		return optional_idx();
	}
	return table.GetStorage().GetTotalRows();
}

vector<FitFanout> KnownFanouts(ClientContext &context, TableCatalogEntry &table) {
	vector<FitFanout> fanouts;
	for (auto &entry : ReferencingKeys(context, table)) {
		auto fk_table = KeyTable(context, entry.first);
		if (fk_table) {
			fanouts.push_back(FitFanout {entry.second, *fk_table, entry.first.column});
		}
	}
	return fanouts;
}

static bool Covers(const Model &model, const vector<string> &columns, const vector<FitFanout> &fanouts) {
	for (auto &fanout : fanouts) {
		if (!model.Find(FanoutName(fanout.fk_table.get().name.GetIdentifierName(), fanout.fk_column)).IsValid()) {
			return false;
		}
	}
	for (auto &column : columns) {
		if (!model.Find(column).IsValid()) {
			return false;
		}
	}
	return true;
}

static bool Fresh(const Model &model, optional_idx rows) {
	if (!rows.IsValid()) {
		return true;
	}
	auto drift = std::abs(double(rows.GetIndex()) - double(model.Rows()));
	return drift <= MAX_DRIFT * double(MaxValue<idx_t>(model.Rows(), 1));
}

static bool Background(ClientContext &context) {
	Value value;
	return context.TryGetCurrentSetting(BACKGROUND_SETTING, value) && value.GetValue<bool>();
}

//! Save a fitted model and the keys learned so far, unless saving is off. A failure leaves
//! the model in memory only.
static void Save(ClientContext &context, TableCatalogEntry &table, const Model &model) {
	Value value;
	if (context.TryGetCurrentSetting(SAVE_SETTING, value) && !value.GetValue<bool>()) {
		return;
	}
	SaveModel(context, table, model);
	SaveKeys(context, table.ParentCatalog());
}

//! The fit of a table: its columns and fanouts as known now. Throws when it fails.
static shared_ptr<Model> FitNow(ClientContext &context, Connection &con, TableCatalogEntry &table) {
	auto columns = FilterColumns(context, table);
	auto fanouts = KnownFanouts(context, table);
	auto fit_columns = columns.empty() ? vector<FitColumn>() : FitColumns(context, table, columns);
	return FitModel(con, table, fit_columns, fanouts, FitOptions());
}

//! Install a background fit, or note its failure.
static void Install(ClientContext &context, TableCatalogEntry &table, shared_ptr<Model> model) {
	auto entry = ObjectCache::GetObjectCache(context).GetOrCreate<CachedModel>(Key(table));
	lock_guard<mutex> guard(entry->lock);
	entry->queued = false;
	if (model) {
		entry->model = std::move(model);
		return;
	}
	entry->failed_rows = CurrentRows(table);
	entry->failed_fanouts = KnownFanouts(context, table).size() + FilterColumns(context, table).size();
}

//! A queue of tables to fit, drained by one thread per database.
//!
//! The thread holds the database only while it fits a table. If it holds the last
//! reference, closing the database destroys this entry on the thread itself: the
//! queue is shared, so the thread sees stop and exits without touching the entry.
struct FitWorker : public ObjectCacheEntry {
	struct Queue {
		mutex lock;
		std::condition_variable wake;
		std::deque<KeyColumn> tables;
		bool stop = false;
	};

	static string ObjectType() {
		return "cardinal_fit_worker";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx();
	}

	~FitWorker() override {
		{
			lock_guard<mutex> guard(queue->lock);
			queue->stop = true;
		}
		queue->wake.notify_all();
		if (!thread.joinable()) {
			return;
		}
		if (thread.get_id() == std::this_thread::get_id()) {
			thread.detach();
		} else {
			thread.join();
		}
	}

	void Enqueue(ClientContext &context, TableCatalogEntry &table) {
		lock_guard<mutex> guard(queue->lock);
		queue->tables.push_back(KeyColumn(table, ""));
		if (!thread.joinable()) {
			thread = std::thread(Run, queue, weak_ptr<DatabaseInstance>(context.db));
		}
		queue->wake.notify_one();
	}

	static void Run(shared_ptr<Queue> queue, weak_ptr<DatabaseInstance> database) {
		// Its queries are inner queries: the extension leaves them alone.
		InnerScope scope;
		while (true) {
			KeyColumn name;
			{
				std::unique_lock<mutex> guard(queue->lock);
				queue->wake.wait(guard, [&] { return queue->stop || !queue->tables.empty(); });
				if (queue->stop) {
					return;
				}
				name = queue->tables.front();
				queue->tables.pop_front();
			}
			auto db = database.lock();
			if (!db) {
				return;
			}
			FitOne(*db, name);
		}
	}

	static void FitOne(DatabaseInstance &db, const KeyColumn &name) {
		Connection con(db);
		auto &context = *con.context;
		optional_ptr<TableCatalogEntry> table;
		shared_ptr<Model> model;
		try {
			context.RunFunctionInTransaction([&]() { table = KeyTable(context, name); });
			if (!table) {
				return;
			}
			model = FitNow(context, con, *table);
		} catch (std::exception &) {
			model = nullptr;
		}
		if (model) {
			Save(context, *table, *model);
		}
		if (table) {
			context.RunFunctionInTransaction([&]() { Install(context, *table, model); });
		}
	}

	shared_ptr<Queue> queue = make_shared_ptr<Queue>();
	std::thread thread;
};

shared_ptr<Model> GetModel(ClientContext &context, TableCatalogEntry &table) {
	// Inner queries leave estimates to DuckDB.
	if (InInnerQuery()) {
		return nullptr;
	}
	auto entry = ObjectCache::GetObjectCache(context).GetOrCreate<CachedModel>(Key(table));
	lock_guard<mutex> guard(entry->lock);
	auto rows = CurrentRows(table);
	auto fanouts = KnownFanouts(context, table);
	auto columns = FilterColumns(context, table);
	if (entry->model && Fresh(*entry->model, rows) && Covers(*entry->model, columns, fanouts)) {
		return entry->model;
	}

	if (!entry->read) {
		entry->read = true;
		auto stored = ReadModel(context, table);
		if (stored) {
			// Its columns are columns queries filtered on when it was fitted.
			vector<string> stored_columns;
			for (auto &column : stored->Columns()) {
				if (table.GetColumns().ColumnExists(Identifier(column.name))) {
					stored_columns.push_back(column.name);
				}
			}
			AddFilterColumns(context, table, stored_columns);
			columns = FilterColumns(context, table);
		}
		if (stored && Fresh(*stored, rows) && Covers(*stored, columns, fanouts)) {
			entry->model = stored;
			return stored;
		}
	}

	// Nothing to model: no filter, no fanout.
	if (columns.empty() && fanouts.empty()) {
		return nullptr;
	}
	if (entry->failed_rows.IsValid() && rows.IsValid() && entry->failed_rows.GetIndex() == rows.GetIndex() &&
	    entry->failed_fanouts == fanouts.size() + columns.size()) {
		return nullptr;
	}

	// In the background: plan with the model there is, possibly stale or none.
	if (Background(context)) {
		if (!entry->queued) {
			entry->queued = true;
			ObjectCache::GetObjectCache(context).GetOrCreate<FitWorker>("cardinal:fit_worker")->Enqueue(context, table);
		}
		return entry->model;
	}

	try {
		InnerScope scope;
		Connection con(*context.db);
		entry->model = FitNow(context, con, table);
		Save(context, table, *entry->model);
	} catch (std::exception &) {
		// e.g. an empty table, or one the fit connection cannot see.
		entry->model = nullptr;
		entry->failed_rows = rows;
		entry->failed_fanouts = fanouts.size() + columns.size();
	}
	return entry->model;
}

void PutModel(ClientContext &context, TableCatalogEntry &table, shared_ptr<Model> model) {
	auto entry = ObjectCache::GetObjectCache(context).GetOrCreate<CachedModel>(Key(table));
	lock_guard<mutex> guard(entry->lock);
	entry->model = std::move(model);
	entry->read = true;
}

} // namespace cardinal
} // namespace duckdb
