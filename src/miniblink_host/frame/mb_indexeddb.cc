#include "miniblink_host/frame/mb_indexeddb.h"

#include <stdint.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/bit_cast.h"
#include "base/containers/span.h"
#include "base/strings/string_number_conversions.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/renderer/modules/indexeddb/idb_key.h"
#include "third_party/blink/renderer/modules/indexeddb/idb_key_range.h"
#include "third_party/blink/renderer/modules/indexeddb/idb_metadata.h"
#include "third_party/blink/renderer/modules/indexeddb/idb_value.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace mb {
namespace {

namespace m = blink::mojom::blink;
using m::IDBDatabaseCallbacks;
using m::IDBFactoryClient;
using m::IDBTransaction;

// One stored record: the primary key (kept so getAll/cursors can report it) + the
// serialized value bytes.
struct MbRecord {
  std::unique_ptr<blink::IDBKey> key;
  std::string bytes;
};

// In-memory backing store for one database name: its current version + schema (object
// store metadata) + the records. Records are keyed by an ORDER-PRESERVING encoding of the
// IDBKey, so std::map iterates them in IndexedDB key order (needed for getAll/ranges).
// Process-wide so reopen sees the same database.
struct MbIDBBackend {
  blink::IDBDatabaseMetadata metadata;  // name/version/object_stores
  std::map<int64_t, std::map<std::string, MbRecord>> data;  // store -> ekey -> record
  std::map<int64_t, int64_t> key_generator;  // store -> next auto-increment value
  // Secondary indexes: store -> index -> (encoded index key -> set of encoded primary keys).
  std::map<int64_t,
           std::map<int64_t, std::map<std::string, std::set<std::string>>>>
      index_data;
  // The active connection's database callbacks (transaction completion is reported here).
  mojo::AssociatedRemote<IDBDatabaseCallbacks> db_callbacks;
};

bool IsAutoIncrement(MbIDBBackend* b, int64_t store_id) {
  auto it = b->metadata.object_stores.find(store_id);
  return it != b->metadata.object_stores.end() && it->value->auto_increment;
}

bool IsUniqueIndex(MbIDBBackend* b, int64_t store_id, int64_t index_id) {
  auto it = b->metadata.object_stores.find(store_id);
  if (it == b->metadata.object_stores.end())
    return false;
  auto ix = it->value->indexes.find(index_id);
  return ix != it->value->indexes.end() && ix->value->unique;
}

// Remove a primary key from every index of a store (on delete / before re-put).
void RemoveFromIndexes(MbIDBBackend* b, int64_t store_id,
                       const std::string& primary_ekey) {
  auto it = b->index_data.find(store_id);
  if (it == b->index_data.end())
    return;
  for (auto& index : it->second)
    for (auto& entry : index.second)
      entry.second.erase(primary_ekey);
}

// Big-endian, order-preserving encoding of a double: flip the sign bit for positives and
// all bits for negatives, so lexicographic byte order matches numeric order.
std::string EncodeDouble(double d) {
  uint64_t u = base::bit_cast<uint64_t>(d);
  u = (u & (uint64_t{1} << 63)) ? ~u : (u | (uint64_t{1} << 63));
  char buf[8];
  for (int i = 0; i < 8; ++i)
    buf[i] = static_cast<char>((u >> (56 - 8 * i)) & 0xFF);
  return std::string(buf, 8);
}

// Encode an IDBKey to a comparable std::string. A leading type-rank byte groups types in
// IndexedDB order (number < date < string < binary); numbers/dates are order-preserving,
// strings/binary compare bytewise. Arrays/none/invalid yield an empty encoding (unsupported).
std::string EncodeKey(const blink::IDBKey* k) {
  if (!k)
    return std::string();
  switch (k->GetType()) {
    case blink::mojom::IDBKeyType::Number:
      return std::string(1, '\x10') + EncodeDouble(k->Number());
    case blink::mojom::IDBKeyType::Date:
      return std::string(1, '\x20') + EncodeDouble(k->Date());
    case blink::mojom::IDBKeyType::String:
      return std::string(1, '\x30') + k->GetString().Utf8();
    case blink::mojom::IDBKeyType::Binary: {
      auto b = k->Binary();
      return std::string(1, '\x40') + std::string(b->data.data(), b->data.size());
    }
    default:
      return std::string();
  }
}

double DecodeDouble(const std::string& s) {
  uint64_t u = 0;
  for (size_t i = 0; i < 8 && i < s.size(); ++i)
    u = (u << 8) | static_cast<uint8_t>(s[i]);
  u = (u & (uint64_t{1} << 63)) ? (u & ~(uint64_t{1} << 63)) : ~u;
  return base::bit_cast<double>(u);
}

// Inverse of EncodeKey, to reconstruct an index key for an index cursor's `key`.
std::unique_ptr<blink::IDBKey> DecodeKey(const std::string& e) {
  if (e.empty())
    return blink::IDBKey::CreateNone();
  std::string rest = e.substr(1);
  switch (e[0]) {
    case '\x10':
      return blink::IDBKey::CreateNumber(DecodeDouble(rest));
    case '\x20':
      return blink::IDBKey::CreateDate(DecodeDouble(rest));
    case '\x30':
      return blink::IDBKey::CreateString(
          blink::String::FromUtf8(std::string_view(rest)));
    default:
      return blink::IDBKey::CreateNone();
  }
}

std::string ValueBytes(const blink::IDBValue* v) {
  base::span<const uint8_t> d = v->Data();
  return std::string(reinterpret_cast<const char*>(d.data()), d.size());
}

blink::IDBKeyPath StoreKeyPath(MbIDBBackend* b, int64_t store_id) {
  auto it = b->metadata.object_stores.find(store_id);
  return it != b->metadata.object_stores.end() ? it->value->key_path
                                               : blink::IDBKeyPath();
}

std::unique_ptr<blink::IDBValue> MakeValue(const std::string& bytes) {
  auto v = std::make_unique<blink::IDBValue>();
  v->SetData(mojo_base::BigBuffer(base::as_byte_span(bytes)));
  return v;
}

// Is an encoded key within [lo, hi] (empty bound = unbounded; *_open = exclusive)?
bool InRange(const std::string& ek,
             const std::string& lo,
             bool lo_open,
             const std::string& hi,
             bool hi_open) {
  if (ek.empty())
    return false;
  if (!lo.empty() && (ek < lo || (lo_open && ek == lo)))
    return false;
  if (!hi.empty() && (ek > hi || (hi_open && ek == hi)))
    return false;
  return true;
}

// Build the IDBReturnValue blink expects (value bytes + primary key + the store's key path
// — the path MUST match the store's or idb_request.cc DCHECKs).
m::IDBReturnValuePtr BuildReturnValue(const MbRecord& rec,
                                      const blink::IDBKeyPath& key_path) {
  auto rv = m::IDBReturnValue::New();
  rv->value = std::make_unique<blink::IDBValue>();
  rv->value->SetData(mojo_base::BigBuffer(base::as_byte_span(rec.bytes)));
  // primary_key is non-nullable. Only in-line-key stores (a String key path) re-inject the
  // key into the deserialized value; for out-of-line stores send a None-typed key, which
  // blink ignores (a real key + a non-String path would DCHECK in the key injector).
  if (key_path.GetType() == blink::mojom::IDBKeyPathType::String) {
    rv->primary_key = blink::IDBKey::Clone(rec.key);
    rv->key_path = key_path;
  } else {
    rv->primary_key = blink::IDBKey::CreateNone();
  }
  return rv;
}

std::map<std::string, std::unique_ptr<MbIDBBackend>>& Registry() {
  static auto* r = new std::map<std::string, std::unique_ptr<MbIDBBackend>>();
  return *r;
}

MbIDBBackend* GetOrCreate(const blink::String& name) {
  std::string key = name.Utf8();
  auto it = Registry().find(key);
  if (it != Registry().end())
    return it->second.get();
  auto b = std::make_unique<MbIDBBackend>();
  b->metadata.name = name;
  b->metadata.version = blink::IDBDatabaseMetadata::kNoVersion;
  MbIDBBackend* p = b.get();
  Registry()[key] = std::move(b);
  return p;
}

int64_t CurrentVersion(const MbIDBBackend* b) {
  return b->metadata.version == blink::IDBDatabaseMetadata::kNoVersion
             ? 0
             : b->metadata.version;
}

// The connection's IDBDatabase. Schema-mutating ops route through transactions, so most of
// this is inert for step 1; reads/writes land in step 2.
class MbIDBTransactionImpl;  // defined below

// A cursor over an object store's records. At open it snapshots the in-range encoded keys in
// iteration order (forward or reverse); each Continue/Advance/Prefetch walks that list and
// looks the live record up by key. `pos_` is the index of the NEXT record to deliver (open
// already delivered keys_[0], so pos_ starts at 1). Self-deletes when its pipe disconnects.
class MbIDBCursor : public m::IDBCursor {
 public:
  // entries: (cursor-key, primary-key) encoded pairs in iteration order. For an object-store
  // cursor the two are equal; for an index cursor the cursor key is the index key.
  MbIDBCursor(MbIDBBackend* backend,
              int64_t store_id,
              bool key_only,
              bool is_index,
              std::vector<std::pair<std::string, std::string>> entries)
      : backend_(backend),
        store_id_(store_id),
        key_only_(key_only),
        is_index_(is_index),
        entries_(std::move(entries)) {}

  void Advance(uint32_t count, AdvanceCallback callback) override {
    if (count > 1)
      pos_ += (count - 1);
    std::move(callback).Run(TakeOne());
  }
  void Continue(std::unique_ptr<blink::IDBKey> key,
                std::unique_ptr<blink::IDBKey> /*primary_key*/,
                ContinueCallback callback) override {
    if (key && key->IsValid()) {
      std::string target = EncodeKey(key.get());  // compared against the cursor key
      while (pos_ < entries_.size() && entries_[pos_].first < target)
        ++pos_;
    }
    std::move(callback).Run(TakeOne());
  }
  void Prefetch(int32_t count, PrefetchCallback callback) override {
    auto cv = m::IDBCursorValue::New();
    int32_t n = 0;
    for (; n < count && pos_ < entries_.size(); ++n)
      if (!AppendCurrent(cv.get()))
        break;
    last_prefetch_ = n;
    if (n == 0) {
      std::move(callback).Run(m::IDBCursorResult::NewEmpty(true));
      return;
    }
    std::move(callback).Run(m::IDBCursorResult::NewValues(std::move(cv)));
  }
  void PrefetchReset(int32_t used_prefetches) override {
    pos_ -= (last_prefetch_ - used_prefetches);
    last_prefetch_ = 0;
  }

  void set_pos(size_t pos) { pos_ = pos; }

 private:
  const std::map<std::string, MbRecord>* StoreMap() const {
    auto it = backend_->data.find(store_id_);
    return it == backend_->data.end() ? nullptr : &it->second;
  }
  // Append entries_[pos_] (advancing pos_) into a cursor value; false if its record is gone.
  bool AppendCurrent(m::IDBCursorValue* cv) {
    const auto* store = StoreMap();
    while (pos_ < entries_.size()) {
      const auto& entry = entries_[pos_];
      auto it = store ? store->find(entry.second) : decltype(store->end())();
      std::string cursor_key_enc = entry.first;
      ++pos_;
      if (store && it != store->end()) {
        cv->keys.push_back(is_index_ ? DecodeKey(cursor_key_enc)
                                     : blink::IDBKey::Clone(it->second.key));
        cv->primary_keys.push_back(blink::IDBKey::Clone(it->second.key));
        if (!key_only_)
          cv->values.push_back(MakeValue(it->second.bytes));
        return true;
      }
    }
    return false;
  }
  m::IDBCursorResultPtr TakeOne() {
    auto cv = m::IDBCursorValue::New();
    if (!AppendCurrent(cv.get()))
      return m::IDBCursorResult::NewEmpty(true);
    return m::IDBCursorResult::NewValues(std::move(cv));
  }

  MbIDBBackend* backend_;
  int64_t store_id_;
  bool key_only_;
  bool is_index_;
  std::vector<std::pair<std::string, std::string>> entries_;  // (cursor-key, primary-key)
  size_t pos_ = 0;
  int32_t last_prefetch_ = 0;
};

class MbIDBDatabase : public m::IDBDatabase {
 public:
  explicit MbIDBDatabase(MbIDBBackend* backend) : backend_(backend) {}

  void RenameObjectStore(int64_t, int64_t, const blink::String&) override {}
  void CreateTransaction(
      mojo::PendingAssociatedReceiver<IDBTransaction> transaction_receiver,
      int64_t transaction_id,
      const blink::Vector<int64_t>&,
      m::IDBTransactionMode,
      m::IDBTransactionDurability) override;
  void VersionChangeIgnored() override {}
  // objectStore.get(key): look the value up by the (only-)key in the range.
  // get()/getKey() on an object store (index_id == kInvalidId) or an index. For an index,
  // the range key is the INDEX key; resolve it to a primary key, then the record.
  void Get(int64_t, int64_t object_store_id, int64_t index_id,
           m::IDBKeyRangePtr key_range, bool key_only,
           GetCallback callback) override {
    std::string ekey = EncodeKey(key_range ? key_range->lower.get() : nullptr);
    auto store_it = backend_->data.find(object_store_id);
    if (ekey.empty() || store_it == backend_->data.end()) {
      std::move(callback).Run(m::IDBDatabaseGetResult::NewEmpty(true));
      return;
    }

    std::string primary_ekey;
    if (index_id == blink::IDBIndexMetadata::kInvalidId) {
      if (!store_it->second.count(ekey)) {
        std::move(callback).Run(m::IDBDatabaseGetResult::NewEmpty(true));
        return;
      }
      primary_ekey = ekey;
    } else {
      // Index lookup: ekey is the index key -> smallest matching primary key.
      auto si = backend_->index_data.find(object_store_id);
      const std::set<std::string>* pkeys =
          (si != backend_->index_data.end() && si->second.count(index_id) &&
           si->second.at(index_id).count(ekey))
              ? &si->second.at(index_id).at(ekey)
              : nullptr;
      if (!pkeys || pkeys->empty() || !store_it->second.count(*pkeys->begin())) {
        std::move(callback).Run(m::IDBDatabaseGetResult::NewEmpty(true));
        return;
      }
      primary_ekey = *pkeys->begin();
    }

    const MbRecord& rec = store_it->second.at(primary_ekey);
    if (key_only) {
      std::move(callback).Run(
          m::IDBDatabaseGetResult::NewKey(blink::IDBKey::Clone(rec.key)));
      return;
    }
    std::move(callback).Run(m::IDBDatabaseGetResult::NewValue(
        BuildReturnValue(rec, StoreKeyPath(backend_, object_store_id))));
  }

  // getAll()/getAllKeys()/getAllRecords(): emit records in IDB key order (the encoded-key
  // map is sorted), honoring the key range, max_count, and direction.
  void GetAll(int64_t, int64_t object_store_id, int64_t index_id,
              m::IDBKeyRangePtr key_range, m::IDBGetAllResultType result_type,
              uint32_t max_count, m::IDBCursorDirection direction,
              GetAllCallback callback) override {
    blink::Vector<m::IDBRecordPtr> records;
    auto store_it = backend_->data.find(object_store_id);
    if (store_it != backend_->data.end()) {
      const auto& store = store_it->second;
      // Encoded-key bounds for the range (order-preserving encoding makes this exact). The
      // range applies to the CURSOR key: the primary key for a store getAll, the INDEX key
      // for an index getAll.
      std::string lo = key_range ? EncodeKey(key_range->lower.get()) : std::string();
      std::string hi = key_range ? EncodeKey(key_range->upper.get()) : std::string();
      const blink::IDBKeyPath kp = StoreKeyPath(backend_, object_store_id);
      const bool keys_only = result_type == m::IDBGetAllResultType::Keys;
      const bool reverse = direction == m::IDBCursorDirection::Prev ||
                           direction == m::IDBCursorDirection::PrevNoDuplicate;
      const uint32_t limit = max_count == 0 ? UINT32_MAX : max_count;
      const bool is_index = index_id != blink::IDBIndexMetadata::kInvalidId;

      auto in_range = [&](const std::string& ek) {
        if (!lo.empty()) {
          if (ek < lo || (key_range->lower_open && ek == lo))
            return false;
        }
        if (!hi.empty()) {
          if (ek > hi || (key_range->upper_open && ek == hi))
            return false;
        }
        return true;
      };
      // Records in forward cursor-key order; reversed below for Prev directions.
      std::vector<const MbRecord*> ordered;
      if (is_index) {
        auto si = backend_->index_data.find(object_store_id);
        if (si != backend_->index_data.end() && si->second.count(index_id)) {
          for (const auto& ik : si->second.at(index_id))  // index key -> primary keys
            if (in_range(ik.first))
              for (const std::string& pk : ik.second) {  // sorted primary keys
                auto rit = store.find(pk);
                if (rit != store.end())
                  ordered.push_back(&rit->second);
              }
        }
      } else {
        for (const auto& entry : store)
          if (in_range(entry.first))
            ordered.push_back(&entry.second);
      }
      if (reverse)
        std::reverse(ordered.begin(), ordered.end());
      for (const MbRecord* rec : ordered) {
        if (records.size() >= limit)
          break;
        auto record = m::IDBRecord::New();
        // Values-only getAll() must NOT carry a primary key (blink CHECKs this); keys are
        // set only for getAllKeys()/getAllRecords().
        if (result_type != m::IDBGetAllResultType::Values)
          record->primary_key = blink::IDBKey::Clone(rec->key);
        if (!keys_only)
          record->return_value = BuildReturnValue(*rec, kp);
        records.push_back(std::move(record));
      }
    }
    std::move(callback).Run(std::move(records), mojo::NullAssociatedReceiver());
  }
  // openCursor()/openKeyCursor(): snapshot the in-range keys in iteration order, hand back a
  // cursor + the first record (or empty).
  void OpenCursor(int64_t, int64_t object_store_id, int64_t index_id,
                  m::IDBKeyRangePtr key_range, m::IDBCursorDirection direction,
                  bool key_only, m::IDBTaskType,
                  OpenCursorCallback callback) override {
    auto store_it = backend_->data.find(object_store_id);
    std::string lo = key_range ? EncodeKey(key_range->lower.get()) : std::string();
    std::string hi = key_range ? EncodeKey(key_range->upper.get()) : std::string();
    bool lo_open = key_range && key_range->lower_open;
    bool hi_open = key_range && key_range->upper_open;
    bool reverse = direction == m::IDBCursorDirection::Prev ||
                   direction == m::IDBCursorDirection::PrevNoDuplicate;
    const bool is_index = index_id != blink::IDBIndexMetadata::kInvalidId;

    // (cursor-key, primary-key) pairs, in cursor-key order; the range applies to the
    // cursor key (primary key for a store cursor, index key for an index cursor).
    std::vector<std::pair<std::string, std::string>> entries;
    if (store_it != backend_->data.end()) {
      if (is_index) {
        auto si = backend_->index_data.find(object_store_id);
        if (si != backend_->index_data.end() && si->second.count(index_id)) {
          for (const auto& ik : si->second.at(index_id))  // index key -> primary keys
            if (InRange(ik.first, lo, lo_open, hi, hi_open))
              for (const std::string& pk : ik.second)  // sorted primary keys
                entries.emplace_back(ik.first, pk);
        }
      } else {
        for (const auto& entry : store_it->second)
          if (InRange(entry.first, lo, lo_open, hi, hi_open))
            entries.emplace_back(entry.first, entry.first);
      }
      if (reverse)
        std::reverse(entries.begin(), entries.end());
    }
    if (entries.empty()) {
      std::move(callback).Run(m::IDBDatabaseOpenCursorResult::NewEmpty(true));
      return;
    }

    const MbRecord& first = store_it->second.at(entries.front().second);
    auto val = m::IDBDatabaseOpenCursorValue::New();
    val->key = is_index ? DecodeKey(entries.front().first)
                        : blink::IDBKey::Clone(first.key);
    val->primary_key = blink::IDBKey::Clone(first.key);
    if (!key_only)
      val->value = MakeValue(first.bytes);

    auto cursor = std::make_unique<MbIDBCursor>(
        backend_, object_store_id, key_only, is_index, std::move(entries));
    cursor->set_pos(1);  // entries[0] is delivered in this open result
    mojo::PendingAssociatedRemote<m::IDBCursor> cursor_remote;
    auto cursor_receiver = cursor_remote.InitWithNewEndpointAndPassReceiver();
    mojo::MakeSelfOwnedAssociatedReceiver(std::move(cursor),
                                          std::move(cursor_receiver));
    val->cursor = std::move(cursor_remote);
    std::move(callback).Run(
        m::IDBDatabaseOpenCursorResult::NewValue(std::move(val)));
  }
  // count(): a single-key range counts that key (0/1); an absent/unbounded range counts
  // every record in the store.
  void Count(int64_t, int64_t object_store_id, int64_t, m::IDBKeyRangePtr key_range,
             CountCallback callback) override {
    auto it = backend_->data.find(object_store_id);
    uint64_t n = 0;
    if (it != backend_->data.end()) {
      std::string ekey =
          EncodeKey(key_range ? key_range->lower.get() : nullptr);
      n = ekey.empty() ? it->second.size() : it->second.count(ekey);
    }
    std::move(callback).Run(/*success=*/true, n);
  }
  // delete(key): remove the single-key record; an unbounded range removes all.
  void DeleteRange(int64_t, int64_t object_store_id, m::IDBKeyRangePtr key_range,
                   DeleteRangeCallback callback) override {
    auto it = backend_->data.find(object_store_id);
    if (it != backend_->data.end()) {
      std::string ekey =
          EncodeKey(key_range ? key_range->lower.get() : nullptr);
      if (ekey.empty()) {
        it->second.clear();
        backend_->index_data[object_store_id].clear();
      } else {
        it->second.erase(ekey);
        RemoveFromIndexes(backend_, object_store_id, ekey);
      }
    }
    std::move(callback).Run(/*success=*/true);
  }
  void GetKeyGeneratorCurrentNumber(
      int64_t, int64_t object_store_id,
      GetKeyGeneratorCurrentNumberCallback callback) override {
    auto it = backend_->key_generator.find(object_store_id);
    int64_t current = it != backend_->key_generator.end() ? it->second : 1;
    std::move(callback).Run(current, nullptr);
  }
  void Clear(int64_t, int64_t object_store_id,
             ClearCallback callback) override {
    auto it = backend_->data.find(object_store_id);
    if (it != backend_->data.end())
      it->second.clear();
    backend_->index_data[object_store_id].clear();
    std::move(callback).Run(/*success=*/true);
  }
  void CreateIndex(
      int64_t, int64_t object_store_id,
      const scoped_refptr<blink::IDBIndexMetadata>& index) override {
    auto it = backend_->metadata.object_stores.find(object_store_id);
    if (it != backend_->metadata.object_stores.end()) {
      it->value->indexes.Set(index->id, index);
      if (index->id > it->value->max_index_id)
        it->value->max_index_id = index->id;
    }
  }
  void DeleteIndex(int64_t, int64_t object_store_id, int64_t index_id) override {
    auto it = backend_->metadata.object_stores.find(object_store_id);
    if (it != backend_->metadata.object_stores.end())
      it->value->indexes.erase(index_id);
    backend_->index_data[object_store_id].erase(index_id);
  }
  void RenameIndex(int64_t, int64_t, int64_t, const blink::String&) override {}
  void Abort(int64_t) override {}
  void DidBecomeInactive() override {}
  void UpdatePriority(int32_t) override {}

 private:
  MbIDBBackend* backend_;
};

// A transaction. For the version-change transaction created during open, CreateObjectStore
// records schema and Commit fires the open success. (Read/write data ops: step 2.)
class MbIDBTransactionImpl : public IDBTransaction {
 public:
  MbIDBTransactionImpl(MbIDBBackend* backend,
                       int64_t txn_id,
                       mojo::AssociatedRemote<IDBFactoryClient> factory_client)
      : backend_(backend),
        txn_id_(txn_id),
        factory_client_(std::move(factory_client)) {}

  // Send the upgrade event carrying the database handle + current schema.
  void SendUpgradeNeeded(mojo::PendingAssociatedRemote<m::IDBDatabase> db,
                         int64_t old_version) {
    factory_client_->UpgradeNeeded(std::move(db), old_version,
                                   m::IDBDataLoss::None, blink::String(""),
                                   backend_->metadata);
  }

  void CreateObjectStore(int64_t object_store_id,
                         const blink::String& name,
                         const blink::IDBKeyPath& key_path,
                         bool auto_increment) override {
    auto os = blink::IDBObjectStoreMetadata::Create();
    os->id = object_store_id;
    os->name = name;
    os->key_path = key_path;
    os->auto_increment = auto_increment;
    backend_->metadata.object_stores.Set(object_store_id, std::move(os));
    if (object_store_id > backend_->metadata.max_object_store_id)
      backend_->metadata.max_object_store_id = object_store_id;
  }
  void DeleteObjectStore(int64_t object_store_id) override {
    backend_->metadata.object_stores.erase(object_store_id);
  }
  void Put(int64_t object_store_id,
           std::unique_ptr<blink::IDBValue> value,
           std::unique_ptr<blink::IDBKey> key,
           m::IDBPutMode,
           blink::Vector<blink::IDBIndexKeys> index_keys,
           PutCallback callback) override {
    // Auto-increment: blink sends a null key for a key-generator store; the backend
    // assigns the next number. An explicit numeric key also bumps the generator.
    std::unique_ptr<blink::IDBKey> generated;
    const blink::IDBKey* use_key = key.get();
    // "No key" includes a None-typed key (blink sends that for a keyless put).
    const bool no_key = !use_key || !use_key->IsValid() ||
                        use_key->GetType() == blink::mojom::IDBKeyType::None;
    if (no_key && IsAutoIncrement(backend_, object_store_id)) {
      int64_t& next = backend_->key_generator[object_store_id];
      if (next < 1)
        next = 1;
      generated = blink::IDBKey::CreateNumber(static_cast<double>(next));
      ++next;
      use_key = generated.get();
    } else if (use_key && use_key->GetType() == blink::mojom::IDBKeyType::Number &&
               IsAutoIncrement(backend_, object_store_id)) {
      int64_t k = static_cast<int64_t>(use_key->Number());
      int64_t& next = backend_->key_generator[object_store_id];
      if (k >= next)
        next = k + 1;
    }

    std::string ekey = EncodeKey(use_key);
    if (ekey.empty()) {
      std::move(callback).Run(m::IDBTransactionPutResult::NewErrorResult(
          m::IDBError::New(m::IDBException::kDataError,
                           blink::String("unsupported key type"))));
      return;
    }
    // Enforce unique-index constraints: reject if a unique index key already maps to a
    // DIFFERENT record.
    for (const blink::IDBIndexKeys& ik : index_keys) {
      if (!IsUniqueIndex(backend_, object_store_id, ik.id))
        continue;
      auto& idx = backend_->index_data[object_store_id][ik.id];
      for (const std::unique_ptr<blink::IDBKey>& ikey : ik.keys) {
        std::string e = EncodeKey(ikey.get());
        auto it = idx.find(e);
        if (it != idx.end() &&
            (it->second.size() > 1 || !it->second.count(ekey))) {
          std::move(callback).Run(m::IDBTransactionPutResult::NewErrorResult(
              m::IDBError::New(m::IDBException::kConstraintError,
                               blink::String("uniqueness constraint violated"))));
          return;
        }
      }
    }

    RemoveFromIndexes(backend_, object_store_id, ekey);  // shed old index entries
    backend_->data[object_store_id][ekey] =
        MbRecord{blink::IDBKey::Clone(use_key), ValueBytes(value.get())};
    // Populate this record's secondary-index entries (blink computes the keys per index).
    for (const blink::IDBIndexKeys& ik : index_keys)
      for (const std::unique_ptr<blink::IDBKey>& ikey : ik.keys) {
        std::string e = EncodeKey(ikey.get());
        if (!e.empty())
          backend_->index_data[object_store_id][ik.id][e].insert(ekey);
      }
    std::move(callback).Run(
        m::IDBTransactionPutResult::NewKey(blink::IDBKey::Clone(use_key)));
  }
  // Populate an index for an existing record (createIndex on a non-empty store).
  void SetIndexKeys(int64_t object_store_id,
                    std::unique_ptr<blink::IDBKey> primary_key,
                    blink::IDBIndexKeys index_keys) override {
    std::string pkey = EncodeKey(primary_key.get());
    if (pkey.empty())
      return;
    for (const std::unique_ptr<blink::IDBKey>& ikey : index_keys.keys) {
      std::string e = EncodeKey(ikey.get());
      if (!e.empty())
        backend_->index_data[object_store_id][index_keys.id][e].insert(pkey);
    }
  }
  void SetIndexKeysDone() override {}
  void Commit(int64_t /*num_errors_handled*/) override {
    // The transaction completed: notify its oncomplete, and (for the open's
    // version-change transaction) resolve the open request.
    if (backend_->db_callbacks)
      backend_->db_callbacks->Complete(txn_id_);
    if (factory_client_) {
      factory_client_->OpenSuccess(mojo::NullAssociatedRemote(),
                                   backend_->metadata);
    }
  }

 private:
  MbIDBBackend* backend_;
  int64_t txn_id_;
  mojo::AssociatedRemote<IDBFactoryClient> factory_client_;
};

void MbIDBDatabase::CreateTransaction(
    mojo::PendingAssociatedReceiver<IDBTransaction> transaction_receiver,
    int64_t transaction_id,
    const blink::Vector<int64_t>&,
    m::IDBTransactionMode,
    m::IDBTransactionDurability) {
  // A regular (non-version-change) transaction: no factory client.
  mojo::MakeSelfOwnedAssociatedReceiver(
      std::make_unique<MbIDBTransactionImpl>(
          backend_, transaction_id, mojo::AssociatedRemote<IDBFactoryClient>()),
      std::move(transaction_receiver));
}

class MbIDBFactory : public m::IDBFactory {
 public:
  void GetDatabaseInfo(GetDatabaseInfoCallback callback) override {
    std::move(callback).Run({}, nullptr);
  }

  void Open(mojo::PendingAssociatedRemote<IDBFactoryClient> client_pending,
            mojo::PendingAssociatedRemote<IDBDatabaseCallbacks> db_cb_pending,
            const blink::String& name,
            int64_t version,
            mojo::PendingAssociatedReceiver<IDBTransaction> vc_txn_receiver,
            int64_t transaction_id,
            int32_t /*priority*/) override {
    MbIDBBackend* backend = GetOrCreate(name);
    mojo::AssociatedRemote<IDBFactoryClient> client(std::move(client_pending));
    // This connection's transaction-completion sink (shared by all its transactions).
    backend->db_callbacks.reset();
    backend->db_callbacks.Bind(std::move(db_cb_pending));

    // The database handle blink will drive.
    mojo::PendingAssociatedRemote<m::IDBDatabase> db_remote;
    auto db_receiver = db_remote.InitWithNewEndpointAndPassReceiver();
    mojo::MakeSelfOwnedAssociatedReceiver(
        std::make_unique<MbIDBDatabase>(backend), std::move(db_receiver));

    const int64_t current = CurrentVersion(backend);
    if (version > current) {
      backend->metadata.version = version;
      auto txn = std::make_unique<MbIDBTransactionImpl>(
          backend, transaction_id, std::move(client));
      MbIDBTransactionImpl* txn_ptr = txn.get();
      mojo::MakeSelfOwnedAssociatedReceiver(std::move(txn),
                                            std::move(vc_txn_receiver));
      // onupgradeneeded -> page creates stores -> vc txn commits -> OpenSuccess.
      txn_ptr->SendUpgradeNeeded(std::move(db_remote), current);
    } else {
      client->OpenSuccess(std::move(db_remote), backend->metadata);
    }
  }

  void DeleteDatabase(
      mojo::PendingAssociatedRemote<IDBFactoryClient> client_pending,
      const blink::String& name,
      bool /*force_close*/) override {
    Registry().erase(name.Utf8());
    mojo::AssociatedRemote<IDBFactoryClient> client(std::move(client_pending));
    client->DeleteSuccess(0);
  }
};

}  // namespace

void BindIDBFactory(
    mojo::PendingReceiver<blink::mojom::blink::IDBFactory> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbIDBFactory>(),
                              std::move(receiver));
}

}  // namespace mb
