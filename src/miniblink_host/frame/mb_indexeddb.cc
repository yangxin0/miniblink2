#include "miniblink_host/frame/mb_indexeddb.h"

#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/bit_cast.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "miniblink_host/frame/mb_frame_origin.h"
#include "miniblink_host/runtime/mb_runtime.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/renderer/modules/indexeddb/idb_key.h"
#include "third_party/blink/renderer/modules/indexeddb/idb_key_range.h"
#include "third_party/blink/renderer/modules/indexeddb/idb_metadata.h"
#include "third_party/blink/renderer/modules/indexeddb/idb_value.h"
#include "base/barrier_closure.h"
#include "base/time/time.h"
#include "miniblink_host/blob/mb_blob_registry.h"
#include "third_party/blink/public/platform/web_blob_info.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace mb {
namespace {

namespace m = blink::mojom::blink;
using m::IDBDatabaseCallbacks;
using m::IDBFactoryClient;
using m::IDBTransaction;

class MbIDBDatabase;         // owns the per-connection IDBDatabaseCallbacks sink
class MbIDBTransactionImpl;  // version-change txn is tracked on the backend

// One stored record: the primary key (kept so getAll/cursors can report it) + the
// serialized value bytes + any attached Blobs/Files. The SSV `bytes` reference blobs
// by index; `blob_info` carries the WebBlobInfo handles (each holds a ref-counted
// BlobDataHandle), so retaining them keeps the in-process MbBlobs alive for the life
// of the record. On read we re-attach them to the IDBValue, and mojo serializes the
// blob handles back to the renderer — so a stored File/Blob reads back intact within
// the session. (Persistence does NOT capture blob bytes — see SerializeRegistry.)
struct MbRecord {
  std::unique_ptr<blink::IDBKey> key;
  std::string bytes;
  blink::Vector<blink::WebBlobInfo> blob_info;
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
  // The in-flight version-change (upgrade) transaction, or null. Tracked so an
  // aborted upgrade can be found from MbIDBDatabase::Abort (to reject the open
  // request + roll back the schema). Cleared on the txn's commit/abort/destruction.
  MbIDBTransactionImpl* version_change_txn = nullptr;
  // Live connections (open MbIDBDatabase handles) to this backend. Used to fire a
  // best-effort versionchange notification on OTHER connections when one of them runs
  // deleteDatabase. Each MbIDBDatabase registers/unregisters itself (ctor/dtor).
  std::set<MbIDBDatabase*> connections;
  // Snapshot of the schema (version + object stores) as it was BEFORE the in-flight
  // upgrade began — the per-transaction data snapshot below does NOT cover metadata,
  // so an aborted upgrade restores this to undo CreateObjectStore/version changes.
  blink::IDBDatabaseMetadata pre_upgrade_metadata;

  // Per-transaction rollback snapshot of the mutable store state, captured lazily and
  // PER STORE on a transaction's first mutation of that store. A subsequent Abort restores
  // only the stores the transaction touched, so two concurrent transactions with disjoint
  // scopes don't clobber each other (a whole-backend snapshot did). Keyed by (connection,
  // transaction id): blink allocates transaction ids per-connection, so two connections can
  // reuse the same id — the connection pointer disambiguates. Commit discards the snapshot.
  struct Snapshot {
    std::map<int64_t, std::map<std::string, MbRecord>> data;  // store -> records
    std::map<int64_t, int64_t> key_generator;                 // store -> next
    std::map<int64_t,
             std::map<int64_t, std::map<std::string, std::set<std::string>>>>
        index_data;             // store -> indexes
    std::set<int64_t> stores;   // which stores this txn captured (present or absent)
  };
  std::map<std::pair<const void*, int64_t>, Snapshot> txn_snapshots;
};

// Deep-clone one store's records (MbRecord holds a unique_ptr<IDBKey>, can't be copied).
std::map<std::string, MbRecord> CloneStore(
    const std::map<std::string, MbRecord>& src) {
  std::map<std::string, MbRecord> out;
  for (const auto& rec : src)
    out[rec.first] = MbRecord{blink::IDBKey::Clone(rec.second.key.get()),
                              rec.second.bytes, rec.second.blob_info};
  return out;
}

// Snapshot store `store_id` before a transaction's first mutation of it (idempotent per
// (conn, txn, store)). Records the store's data + indexes + key generator — or its ABSENCE
// (via `stores`) so a rollback can re-delete a store the txn created.
void EnsureSnapshot(MbIDBBackend* b, const void* conn, int64_t txn_id,
                    int64_t store_id) {
  auto& snap = b->txn_snapshots[{conn, txn_id}];
  if (!snap.stores.insert(store_id).second)
    return;  // already captured this store for this txn
  if (auto dit = b->data.find(store_id); dit != b->data.end())
    snap.data[store_id] = CloneStore(dit->second);
  if (auto iit = b->index_data.find(store_id); iit != b->index_data.end())
    snap.index_data[store_id] = iit->second;
  if (auto kit = b->key_generator.find(store_id); kit != b->key_generator.end())
    snap.key_generator[store_id] = kit->second;
}

// Restore (and consume) a transaction's snapshot — its writes never happened. Only the
// captured stores are reverted; stores absent at snapshot time are erased.
void RollbackSnapshot(MbIDBBackend* b, const void* conn, int64_t txn_id) {
  auto it = b->txn_snapshots.find({conn, txn_id});
  if (it == b->txn_snapshots.end())
    return;
  for (int64_t sid : it->second.stores) {
    if (auto d = it->second.data.find(sid); d != it->second.data.end())
      b->data[sid] = CloneStore(d->second);
    else
      b->data.erase(sid);
    if (auto i = it->second.index_data.find(sid); i != it->second.index_data.end())
      b->index_data[sid] = i->second;
    else
      b->index_data.erase(sid);
    if (auto k = it->second.key_generator.find(sid);
        k != it->second.key_generator.end())
      b->key_generator[sid] = k->second;
    else
      b->key_generator.erase(sid);
  }
  b->txn_snapshots.erase(it);
}

void DiscardSnapshot(MbIDBBackend* b, const void* conn, int64_t txn_id) {
  b->txn_snapshots.erase({conn, txn_id});
}

// Deep-clone database metadata for the pre-upgrade schema snapshot. Object-store
// metadata is mutated IN PLACE by CreateIndex/DeleteIndex (its indexes map +
// max_index_id), so each store is cloned into a FRESH object; index metadata is
// immutable once created, so those scoped_refptrs can be shared.
blink::IDBDatabaseMetadata CloneMetadata(const blink::IDBDatabaseMetadata& md) {
  blink::IDBDatabaseMetadata out;
  out.name = md.name;
  out.version = md.version;
  out.max_object_store_id = md.max_object_store_id;
  for (const auto& [sid, os] : md.object_stores) {
    auto os_copy = blink::IDBObjectStoreMetadata::Create();
    os_copy->id = os->id;
    os_copy->name = os->name;
    os_copy->key_path = os->key_path;
    os_copy->auto_increment = os->auto_increment;
    os_copy->max_index_id = os->max_index_id;
    for (const auto& [iid, idx] : os->indexes)
      os_copy->indexes.Set(iid, idx);
    out.object_stores.Set(sid, std::move(os_copy));
  }
  return out;
}

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
  // IndexedDB treats -0 and +0 as equal, but they have different bit patterns;
  // normalize so they encode identically (else a key stored as -0 wouldn't match +0).
  if (d == 0.0)
    d = 0.0;
  uint64_t u = base::bit_cast<uint64_t>(d);
  u = (u & (uint64_t{1} << 63)) ? ~u : (u | (uint64_t{1} << 63));
  char buf[8];
  for (int i = 0; i < 8; ++i)
    buf[i] = static_cast<char>((u >> (56 - 8 * i)) & 0xFF);
  return std::string(buf, 8);
}

// Make a component self-delimiting for use as an array element: escape every 0x00 byte as
// 0x00 0x01, then append a 0x00 0x00 terminator. The terminator sorts below an escaped null
// (0x00 0x01) and below any element's leading type byte (>= 0x10), so a shorter element/array
// that is a prefix of another sorts first — exactly IndexedDB array order.
std::string EscapeComponent(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    out.push_back(c);
    if (c == '\x00')
      out.push_back('\x01');
  }
  out.push_back('\x00');
  out.push_back('\x00');
  return out;
}

// Encode an IDBKey to a comparable std::string. A leading type-rank byte groups types in
// IndexedDB order (number < date < string < binary < array); numbers/dates are order-
// preserving, strings/binary compare bytewise, arrays compare element-wise (each element
// escaped+terminated, array ended by a 0x00 marker). None/invalid yield an empty encoding.
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
    case blink::mojom::IDBKeyType::Array: {
      std::string out(1, '\x50');
      for (const std::unique_ptr<blink::IDBKey>& e : k->Array()) {
        std::string enc = EncodeKey(e.get());  // recursive (nested arrays ok)
        if (enc.empty())
          return std::string();  // an unsupported element makes the whole key unsupported
        out += EscapeComponent(enc);
      }
      out.push_back('\x00');  // array end (sorts before any element's type byte)
      return out;
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
    case '\x40': {
      // Binary (ArrayBuffer) key — the bytes follow the type byte verbatim (EncodeKey
      // emits them raw). Without this case a persisted binary key reloaded to a None
      // key, so its record reported a bogus primary key after MbLoadIndexedDB.
      auto binary =
          base::MakeRefCounted<base::RefCountedData<blink::Vector<char>>>();
      binary->data.reserve(static_cast<blink::wtf_size_t>(rest.size()));
      for (char c : rest)
        binary->data.push_back(c);
      return blink::IDBKey::CreateBinary(std::move(binary));
    }
    case '\x50': {
      // Array: walk escaped+terminated components until the 0x00 array-end marker.
      blink::IDBKey::KeyArray arr;
      size_t i = 1;
      while (i < e.size() && e[i] != '\x00') {  // 0x00 here is the array end
        std::string comp;
        while (i + 1 < e.size()) {
          if (e[i] == '\x00' && e[i + 1] == '\x00') {  // component terminator
            i += 2;
            break;
          }
          if (e[i] == '\x00' && e[i + 1] == '\x01') {  // escaped null
            comp.push_back('\x00');
            i += 2;
            continue;
          }
          comp.push_back(e[i]);
          ++i;
        }
        arr.push_back(DecodeKey(comp));  // recursive
      }
      return blink::IDBKey::CreateArray(std::move(arr));
    }
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

std::unique_ptr<blink::IDBValue> MakeValue(
    const std::string& bytes,
    const blink::Vector<blink::WebBlobInfo>& blob_info = {}) {
  auto v = std::make_unique<blink::IDBValue>();
  v->SetData(mojo_base::BigBuffer(base::as_byte_span(bytes)));
  if (!blob_info.empty())
    v->SetBlobInfo(blob_info);  // re-attach stored Blobs/Files (handles still live)
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
  if (!rec.blob_info.empty())
    rv->value->SetBlobInfo(rec.blob_info);  // re-attach stored Blobs/Files
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

// Backends removed from the active Registry (by deleteDatabase, or replaced by a
// load-from-disk) are RETIRED here, not freed: a page may still hold a live
// MbIDBDatabase / transaction / cursor whose raw backend_ points into one. Freeing
// it was a use-after-free. Kept for the process lifetime (leaked like the other
// process-global singletons; bounded by how often a DB is deleted/reloaded).
std::vector<std::unique_ptr<MbIDBBackend>>& Graveyard() {
  static auto* g = new std::vector<std::unique_ptr<MbIDBBackend>>();
  return *g;
}
void RetireBackend(std::unique_ptr<MbIDBBackend> b) {
  if (b)
    Graveyard().push_back(std::move(b));
}

// The Registry key qualifies the database name with the opening frame's ORIGIN,
// so two different origins that open the same db name get SEPARATE backends (IDB
// is strictly per-origin). `origin` is "" for an unknown origin (worker): all
// such share one unscoped bucket (a documented residual). The page-visible name
// stays `b->metadata.name` (set below), independent of this composite key.
std::string IDBKey(const std::string& origin, const blink::String& name) {
  return origin + "\n" + name.Utf8();
}

MbIDBBackend* GetOrCreate(const std::string& origin, const blink::String& name) {
  std::string key = IDBKey(origin, name);
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
              bool reverse,
              std::vector<std::pair<std::string, std::string>> entries)
      : backend_(backend),
        store_id_(store_id),
        key_only_(key_only),
        is_index_(is_index),
        reverse_(reverse),
        entries_(std::move(entries)) {}

  void Advance(uint32_t count, AdvanceCallback callback) override {
    if (count > 1)
      pos_ += (count - 1);
    std::move(callback).Run(TakeOne());
  }
  void Continue(std::unique_ptr<blink::IDBKey> key,
                std::unique_ptr<blink::IDBKey> primary_key,
                ContinueCallback callback) override {
    if (key && key->IsValid()) {
      std::string target = EncodeKey(key.get());  // compared against the cursor key
      // continuePrimaryKey(key, primaryKey): among duplicate index keys, seek to the
      // requested PRIMARY key too (empty when plain continue(key) was called). Without
      // this an index cursor landed on the first duplicate, delivering the wrong record.
      std::string ptarget =
          (primary_key && primary_key->IsValid()) ? EncodeKey(primary_key.get())
                                                  : std::string();
      // Skip toward (target, ptarget) in the cursor's iteration direction: ascending for a
      // forward cursor, DESCENDING for a Prev cursor (entries_ is pre-reversed, so an
      // ascending test would walk the wrong way and mis-seek).
      if (reverse_) {
        while (pos_ < entries_.size() &&
               (entries_[pos_].first > target ||
                (!ptarget.empty() && entries_[pos_].first == target &&
                 entries_[pos_].second > ptarget)))
          ++pos_;
      } else {
        while (pos_ < entries_.size() &&
               (entries_[pos_].first < target ||
                (!ptarget.empty() && entries_[pos_].first == target &&
                 entries_[pos_].second < ptarget)))
          ++pos_;
      }
    }
    std::move(callback).Run(TakeOne());
  }
  void Prefetch(int32_t count, PrefetchCallback callback) override {
    auto cv = m::IDBCursorValue::New();
    // AppendCurrent skips stale records, so pos_ does NOT advance by exactly one per
    // delivered value. Record pos_ before the batch and after each delivered value so a
    // reset can rewind precisely by consumed-count rather than guessing the delta.
    prefetch_start_pos_ = pos_;
    prefetch_positions_.clear();
    int32_t n = 0;
    for (; n < count && pos_ < entries_.size(); ++n) {
      if (!AppendCurrent(cv.get()))
        break;
      prefetch_positions_.push_back(pos_);  // pos_ right after consuming this value
    }
    if (n == 0) {
      std::move(callback).Run(m::IDBCursorResult::NewEmpty(true));
      return;
    }
    std::move(callback).Run(m::IDBCursorResult::NewValues(std::move(cv)));
  }
  void PrefetchReset(int32_t used_prefetches) override {
    // Rewind to the position right after the LAST used value (or to the pre-prefetch
    // position if none were used). Relative to consumed count, robust to skipped records.
    if (used_prefetches > 0 &&
        static_cast<size_t>(used_prefetches) <= prefetch_positions_.size())
      pos_ = prefetch_positions_[used_prefetches - 1];
    else
      pos_ = prefetch_start_pos_;
    prefetch_positions_.clear();
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
          cv->values.push_back(
              MakeValue(it->second.bytes, it->second.blob_info));
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
  bool reverse_;  // Prev/PrevNoDuplicate: entries_ is pre-reversed (descending)
  std::vector<std::pair<std::string, std::string>> entries_;  // (cursor-key, primary-key)
  size_t pos_ = 0;
  size_t prefetch_start_pos_ = 0;        // pos_ at the start of the last Prefetch
  std::vector<size_t> prefetch_positions_;  // pos_ after each delivered prefetch value
};

class MbIDBDatabase : public m::IDBDatabase {
 public:
  MbIDBDatabase(MbIDBBackend* backend,
                mojo::PendingAssociatedRemote<IDBDatabaseCallbacks> db_cb)
      : backend_(backend) {
    if (db_cb.is_valid())
      db_callbacks_.Bind(std::move(db_cb));
    backend_->connections.insert(this);
  }
  ~MbIDBDatabase() override { backend_->connections.erase(this); }

  // This connection's transaction-completion sink — PER-CONNECTION, not shared on
  // the backend, so a second connection to the same DB doesn't clobber this one's
  // routing (Complete/Abort reach the connection that owns the transaction).
  IDBDatabaseCallbacks* callbacks() {
    return db_callbacks_.is_bound() ? db_callbacks_.get() : nullptr;
  }
  base::WeakPtr<MbIDBDatabase> AsWeakPtr() { return weak_factory_.GetWeakPtr(); }

  void RenameObjectStore(int64_t, int64_t, const blink::String&) override {}
  void CreateTransaction(
      mojo::PendingAssociatedReceiver<IDBTransaction> transaction_receiver,
      int64_t transaction_id,
      const blink::Vector<int64_t>&,
      m::IDBTransactionMode,
      m::IDBTransactionDurability) override;
  void VersionChangeIgnored() override {}
  // get()/getKey() on an object store (index_id == kInvalidId) or an index, by a key
  // RANGE: return the first record whose (cursor) key falls within the range, in forward
  // key order — not just an exact match on the range's lower bound (which dropped
  // get(lowerBound(5)) when key 5 was absent but 6,7… existed).
  void Get(int64_t, int64_t object_store_id, int64_t index_id,
           m::IDBKeyRangePtr key_range, bool key_only,
           GetCallback callback) override {
    auto store_it = backend_->data.find(object_store_id);
    if (!key_range || store_it == backend_->data.end()) {
      std::move(callback).Run(m::IDBDatabaseGetResult::NewEmpty(true));
      return;
    }
    const std::string lo = EncodeKey(key_range->lower.get());
    const std::string hi = EncodeKey(key_range->upper.get());
    const bool lo_open = key_range->lower_open;
    const bool hi_open = key_range->upper_open;

    std::string primary_ekey;
    bool found = false;
    if (index_id == blink::IDBIndexMetadata::kInvalidId) {
      for (const auto& entry : store_it->second)  // sorted by primary key
        if (InRange(entry.first, lo, lo_open, hi, hi_open)) {
          primary_ekey = entry.first;
          found = true;
          break;
        }
    } else {
      // Index lookup: walk index keys in range, take the smallest primary key whose
      // record still exists.
      auto si = backend_->index_data.find(object_store_id);
      if (si != backend_->index_data.end() && si->second.count(index_id)) {
        for (const auto& ik : si->second.at(index_id)) {  // sorted by index key
          if (!InRange(ik.first, lo, lo_open, hi, hi_open))
            continue;
          for (const std::string& pk : ik.second)  // sorted primary keys
            if (store_it->second.count(pk)) {
              primary_ekey = pk;
              found = true;
              break;
            }
          if (found)
            break;
        }
      }
    }
    if (!found) {
      std::move(callback).Run(m::IDBDatabaseGetResult::NewEmpty(true));
      return;
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
      // nextunique/prevunique: emit only ONE record per index key (the first existing
      // primary key). Only meaningful for an index cursor (store keys are already unique).
      const bool unique =
          direction == m::IDBCursorDirection::NextNoDuplicate ||
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
                if (rit != store.end()) {
                  ordered.push_back(&rit->second);
                  if (unique)
                    break;  // one record per index key
                }
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
    // nextunique/prevunique: one entry per index key (the first primary key). Only
    // meaningful for an index cursor — a store cursor's keys are already unique.
    const bool unique = direction == m::IDBCursorDirection::NextNoDuplicate ||
                        direction == m::IDBCursorDirection::PrevNoDuplicate;
    const bool is_index = index_id != blink::IDBIndexMetadata::kInvalidId;

    // (cursor-key, primary-key) pairs, in cursor-key order; the range applies to the
    // cursor key (primary key for a store cursor, index key for an index cursor).
    std::vector<std::pair<std::string, std::string>> entries;
    if (store_it != backend_->data.end()) {
      if (is_index) {
        auto si = backend_->index_data.find(object_store_id);
        if (si != backend_->index_data.end() && si->second.count(index_id)) {
          for (const auto& ik : si->second.at(index_id)) {  // index key -> primary keys
            if (!InRange(ik.first, lo, lo_open, hi, hi_open))
              continue;
            for (const std::string& pk : ik.second) {  // sorted primary keys
              entries.emplace_back(ik.first, pk);
              if (unique)
                break;  // one entry per index key
            }
          }
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

    // Skip leading entries whose record was deleted (an index entry can outlive its
    // record); .at() below would otherwise throw. Mirrors AppendCurrent's guard.
    size_t start = 0;
    while (start < entries.size() &&
           store_it->second.find(entries[start].second) == store_it->second.end())
      ++start;
    if (start >= entries.size()) {
      std::move(callback).Run(m::IDBDatabaseOpenCursorResult::NewEmpty(true));
      return;
    }

    const MbRecord& first = store_it->second.at(entries[start].second);
    auto val = m::IDBDatabaseOpenCursorValue::New();
    val->key = is_index ? DecodeKey(entries[start].first)
                        : blink::IDBKey::Clone(first.key);
    val->primary_key = blink::IDBKey::Clone(first.key);
    if (!key_only)
      val->value = MakeValue(first.bytes, first.blob_info);

    auto cursor = std::make_unique<MbIDBCursor>(
        backend_, object_store_id, key_only, is_index, reverse,
        std::move(entries));
    cursor->set_pos(start + 1);  // entries[start] is delivered in this open result
    mojo::PendingAssociatedRemote<m::IDBCursor> cursor_remote;
    auto cursor_receiver = cursor_remote.InitWithNewEndpointAndPassReceiver();
    mojo::MakeSelfOwnedAssociatedReceiver(std::move(cursor),
                                          std::move(cursor_receiver));
    val->cursor = std::move(cursor_remote);
    std::move(callback).Run(
        m::IDBDatabaseOpenCursorResult::NewValue(std::move(val)));
  }
  // count(): count records whose (cursor) key is in the range — every record for an
  // absent/unbounded range. Honors the index id (count() on an index counts its entries
  // in range, not the whole store).
  void Count(int64_t, int64_t object_store_id, int64_t index_id,
             m::IDBKeyRangePtr key_range, CountCallback callback) override {
    auto it = backend_->data.find(object_store_id);
    uint64_t n = 0;
    if (it != backend_->data.end()) {
      const std::string lo = key_range ? EncodeKey(key_range->lower.get()) : std::string();
      const std::string hi = key_range ? EncodeKey(key_range->upper.get()) : std::string();
      const bool lo_open = key_range && key_range->lower_open;
      const bool hi_open = key_range && key_range->upper_open;
      if (index_id == blink::IDBIndexMetadata::kInvalidId) {
        if (lo.empty() && hi.empty()) {
          n = it->second.size();
        } else {
          for (const auto& entry : it->second)
            if (InRange(entry.first, lo, lo_open, hi, hi_open))
              ++n;
        }
      } else {
        auto si = backend_->index_data.find(object_store_id);
        if (si != backend_->index_data.end() && si->second.count(index_id))
          for (const auto& ik : si->second.at(index_id))
            if (InRange(ik.first, lo, lo_open, hi, hi_open))
              for (const std::string& pk : ik.second)
                if (it->second.count(pk))
                  ++n;
      }
    }
    std::move(callback).Run(/*success=*/true, n);
  }
  // delete(range): remove every record whose key is in the range; an unbounded range
  // removes all. (Previously only the range's lower-bound key was deleted, silently
  // leaving the rest of a bounded range like delete(bound(1,10)) in place.)
  void DeleteRange(int64_t transaction_id, int64_t object_store_id,
                   m::IDBKeyRangePtr key_range,
                   DeleteRangeCallback callback) override {
    EnsureSnapshot(backend_, this, transaction_id, object_store_id);
    auto it = backend_->data.find(object_store_id);
    if (it != backend_->data.end()) {
      const std::string lo = key_range ? EncodeKey(key_range->lower.get()) : std::string();
      const std::string hi = key_range ? EncodeKey(key_range->upper.get()) : std::string();
      const bool lo_open = key_range && key_range->lower_open;
      const bool hi_open = key_range && key_range->upper_open;
      if (lo.empty() && hi.empty()) {
        it->second.clear();
        backend_->index_data[object_store_id].clear();
      } else {
        for (auto rit = it->second.begin(); rit != it->second.end();) {
          if (InRange(rit->first, lo, lo_open, hi, hi_open)) {
            RemoveFromIndexes(backend_, object_store_id, rit->first);
            rit = it->second.erase(rit);
          } else {
            ++rit;
          }
        }
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
  void Clear(int64_t transaction_id, int64_t object_store_id,
             ClearCallback callback) override {
    EnsureSnapshot(backend_, this, transaction_id, object_store_id);
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
  void DeleteIndex(int64_t transaction_id, int64_t object_store_id,
                   int64_t index_id) override {
    // Snapshot the store before erasing index_data so an aborted upgrade restores the
    // index's entries (metadata rolls back via pre_upgrade_metadata; the index DATA only
    // rolls back through the per-store snapshot). conn == `this`, matching the version-
    // change txn's snapshot key (db_.get() inside the transaction is this same object).
    EnsureSnapshot(backend_, this, transaction_id, object_store_id);
    auto it = backend_->metadata.object_stores.find(object_store_id);
    if (it != backend_->metadata.object_stores.end())
      it->value->indexes.erase(index_id);
    backend_->index_data[object_store_id].erase(index_id);
  }
  void RenameIndex(int64_t, int64_t, int64_t, const blink::String&) override {}
  // transaction.abort() (or an unhandled request error). Defined out-of-line (below
  // MbIDBTransactionImpl) because the version-change case needs the full txn type.
  void Abort(int64_t transaction_id) override;
  void DidBecomeInactive() override {}
  void UpdatePriority(int32_t) override {}

 private:
  MbIDBBackend* backend_;
  mojo::AssociatedRemote<IDBDatabaseCallbacks> db_callbacks_;
  base::WeakPtrFactory<MbIDBDatabase> weak_factory_{this};
};

// A transaction. For the version-change transaction created during open, CreateObjectStore
// records schema and Commit fires the open success. (Read/write data ops: step 2.)
class MbIDBTransactionImpl : public IDBTransaction {
 public:
  MbIDBTransactionImpl(MbIDBBackend* backend,
                       int64_t txn_id,
                       mojo::AssociatedRemote<IDBFactoryClient> factory_client,
                       base::WeakPtr<MbIDBDatabase> db)
      : backend_(backend),
        txn_id_(txn_id),
        factory_client_(std::move(factory_client)),
        db_(std::move(db)) {}
  ~MbIDBTransactionImpl() override {
    // Don't leave the backend pointing at a freed version-change transaction.
    if (backend_->version_change_txn == this)
      backend_->version_change_txn = nullptr;
  }

  int64_t txn_id() const { return txn_id_; }

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
    // Snapshot the store's records BEFORE erasing them so an aborted upgrade rolls them
    // back (the metadata is restored from pre_upgrade_metadata, but the per-txn snapshot
    // is what restores the data — without this an aborted delete lost the records).
    EnsureSnapshot(backend_, db_.get(), txn_id_, object_store_id);
    backend_->metadata.object_stores.erase(object_store_id);
    backend_->data.erase(object_store_id);        // also drop the store's records
    backend_->index_data.erase(object_store_id);  // ...and its secondary-index data
  }
  void Put(int64_t object_store_id,
           std::unique_ptr<blink::IDBValue> value,
           std::unique_ptr<blink::IDBKey> key,
           m::IDBPutMode put_mode,
           blink::Vector<blink::IDBIndexKeys> index_keys,
           PutCallback callback) override {
    // first mutation of this store arms its transaction rollback snapshot
    EnsureSnapshot(backend_, db_.get(), txn_id_, object_store_id);
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
      // Only a NUMBER key bumps the generator (spec). Cap at 2^53 and use a saturating
      // cast: static_cast<int64_t> of a huge double (e.g. 1e30) is undefined behavior.
      double v = std::min(use_key->Number(), 9007199254740992.0);
      int64_t candidate = 1 + base::saturated_cast<int64_t>(std::floor(v));
      int64_t& next = backend_->key_generator[object_store_id];
      if (candidate > next)
        next = candidate;
    }

    std::string ekey = EncodeKey(use_key);
    if (ekey.empty()) {
      std::move(callback).Run(m::IDBTransactionPutResult::NewErrorResult(
          m::IDBError::New(m::IDBException::kDataError,
                           blink::String("unsupported key type"))));
      return;
    }
    // add() (AddOnly) must FAIL with ConstraintError if the primary key already exists —
    // unlike put() (AddOrUpdate), which overwrites. (Previously the put mode was ignored, so
    // add() silently overwrote and never triggered the error-driven transaction abort/rollback.)
    if (put_mode == m::IDBPutMode::AddOnly) {
      auto store = backend_->data.find(object_store_id);
      if (store != backend_->data.end() && store->second.count(ekey)) {
        std::move(callback).Run(m::IDBTransactionPutResult::NewErrorResult(
            m::IDBError::New(m::IDBException::kConstraintError,
                             blink::String("Key already exists in the object store"))));
        return;
      }
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
        MbRecord{blink::IDBKey::Clone(use_key), ValueBytes(value.get()),
                 value->BlobInfo()};  // retain attached Blobs/Files (keeps them alive)
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
    // Snapshot the store before writing index_data so an aborted upgrade rolls these
    // index entries back (index_data is part of the per-store snapshot).
    EnsureSnapshot(backend_, db_.get(), txn_id_, object_store_id);
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
    // changes are now durable; nothing to roll back
    DiscardSnapshot(backend_, db_.get(), txn_id_);
    if (backend_->version_change_txn == this)
      backend_->version_change_txn = nullptr;  // upgrade committed
    // The transaction completed: notify its oncomplete via the OWNING connection's
    // callbacks (per-connection, so a second connection isn't misrouted), and (for
    // the open's version-change transaction) resolve the open request.
    if (db_) {
      if (auto* cb = db_->callbacks())
        cb->Complete(txn_id_);
    }
    if (factory_client_) {
      factory_client_->OpenSuccess(mojo::NullAssociatedRemote(),
                                   backend_->metadata);
    }
  }

 private:
  MbIDBBackend* backend_;
  int64_t txn_id_;
  mojo::AssociatedRemote<IDBFactoryClient> factory_client_;
  base::WeakPtr<MbIDBDatabase> db_;  // owning connection (for its callbacks sink)
};

// Out-of-line (needs the full MbIDBTransactionImpl type for the upgrade case).
void MbIDBDatabase::Abort(int64_t transaction_id) {
  RollbackSnapshot(backend_, this, transaction_id);  // undo this txn's data writes
  // If this is the in-flight version-change (upgrade) transaction, the per-txn data
  // snapshot does NOT cover schema — restore the pre-upgrade metadata (version +
  // object stores) so a later reopen sees the OLD schema, not the half-built one.
  if (backend_->version_change_txn &&
      backend_->version_change_txn->txn_id() == transaction_id) {
    backend_->metadata = backend_->pre_upgrade_metadata;
    backend_->version_change_txn = nullptr;
  }
  // Signal the abort through the connection's IDBDatabaseCallbacks. For an UPGRADE
  // transaction this is ALSO how blink rejects the pending open() (it fires the
  // request's onerror). Do NOT use IDBFactoryClient::Error here: once upgradeneeded
  // has fired, the open request is past PENDING and Error trips a DCHECK in
  // IDBRequest (ready_state_ == PENDING).
  if (auto* cb = callbacks())
    cb->Abort(transaction_id, m::IDBException::kAbortError,
              blink::String("transaction aborted"));
}

void MbIDBDatabase::CreateTransaction(
    mojo::PendingAssociatedReceiver<IDBTransaction> transaction_receiver,
    int64_t transaction_id,
    const blink::Vector<int64_t>&,
    m::IDBTransactionMode,
    m::IDBTransactionDurability) {
  // A regular (non-version-change) transaction: no factory client. It routes
  // completion through THIS connection (weak ptr) so concurrent connections don't
  // cross-talk.
  mojo::MakeSelfOwnedAssociatedReceiver(
      std::make_unique<MbIDBTransactionImpl>(
          backend_, transaction_id, mojo::AssociatedRemote<IDBFactoryClient>(),
          weak_factory_.GetWeakPtr()),
      std::move(transaction_receiver));
}

class MbIDBFactory : public m::IDBFactory {
 public:
  explicit MbIDBFactory(uint64_t frame_key) : frame_key_(frame_key) {}

  void GetDatabaseInfo(GetDatabaseInfoCallback callback) override {
    // databases(): report every database for THIS frame's origin. Registry keys are
    // "origin\nname" (see IDBKey), so filter by the origin prefix. Skip backends that
    // were opened but never upgraded (still kNoVersion) — they aren't real databases yet.
    blink::Vector<m::IDBNameAndVersionPtr> names;
    const std::string prefix = MbGetFrameOrigin(frame_key_) + "\n";
    for (const auto& [key, b] : Registry()) {
      if (key.compare(0, prefix.size(), prefix) != 0)
        continue;
      if (b->metadata.version == blink::IDBDatabaseMetadata::kNoVersion)
        continue;
      names.push_back(
          m::IDBNameAndVersion::New(b->metadata.name, b->metadata.version));
    }
    std::move(callback).Run(std::move(names), nullptr);
  }

  void Open(mojo::PendingAssociatedRemote<IDBFactoryClient> client_pending,
            mojo::PendingAssociatedRemote<IDBDatabaseCallbacks> db_cb_pending,
            const blink::String& name,
            int64_t version,
            mojo::PendingAssociatedReceiver<IDBTransaction> vc_txn_receiver,
            int64_t transaction_id,
            int32_t /*priority*/) override {
    MbIDBBackend* backend = GetOrCreate(MbGetFrameOrigin(frame_key_), name);
    mojo::AssociatedRemote<IDBFactoryClient> client(std::move(client_pending));

    // The database handle blink will drive. Its IDBDatabaseCallbacks sink is bound
    // PER-CONNECTION (inside MbIDBDatabase) rather than on the shared backend, so a
    // second connection to the same DB no longer clobbers this one's routing.
    mojo::PendingAssociatedRemote<m::IDBDatabase> db_remote;
    auto db_receiver = db_remote.InitWithNewEndpointAndPassReceiver();
    auto db_impl =
        std::make_unique<MbIDBDatabase>(backend, std::move(db_cb_pending));
    base::WeakPtr<MbIDBDatabase> db_weak = db_impl->AsWeakPtr();
    mojo::MakeSelfOwnedAssociatedReceiver(std::move(db_impl),
                                          std::move(db_receiver));

    const int64_t current = CurrentVersion(backend);
    // Resolve the effective version, matching content's connection_coordinator:
    //  - open(name) with no version (kNoVersion) -> max(1, current): a brand-new DB
    //    upgrades to 1 (so upgradeneeded fires and stores can be created), an existing
    //    DB just opens at its current version.
    //  - open(name, v) with v < current -> reject with VersionError.
    //  - otherwise upgrade only when the effective version is greater than current.
    int64_t new_version = version;
    if (version == blink::IDBDatabaseMetadata::kNoVersion) {
      new_version = std::max<int64_t>(1, current);
    } else if (version < current) {
      client->Error(
          m::IDBException::kVersionError,
          blink::String("The requested version is less than the existing version"));
      return;
    }
    if (new_version > current) {
      // Snapshot the schema BEFORE the upgrade so an aborted upgrade can roll back
      // version + object stores (the per-txn data snapshot doesn't cover metadata).
      backend->pre_upgrade_metadata = CloneMetadata(backend->metadata);
      backend->metadata.version = new_version;
      auto txn = std::make_unique<MbIDBTransactionImpl>(
          backend, transaction_id, std::move(client), db_weak);
      MbIDBTransactionImpl* txn_ptr = txn.get();
      backend->version_change_txn = txn_ptr;  // tracked for abort (reject + rollback)
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
    // RETIRE the backend rather than erase/free it: a page may still hold a live
    // IDB handle whose raw backend_ points here (deleteDatabase doesn't force-close
    // open connections in this model). Freeing was a use-after-free. A later open()
    // makes a fresh backend (correct: a deleted DB reopens empty).
    int64_t old_version = 0;
    const std::string key = IDBKey(MbGetFrameOrigin(frame_key_), name);
    auto it = Registry().find(key);
    if (it != Registry().end()) {
      old_version = CurrentVersion(it->second.get());
      // Best-effort versionchange: notify every live connection so its onversionchange
      // can react (a real browser would block the delete until they close; here we just
      // signal and proceed). new_version == kNoVersion signals deletion.
      for (MbIDBDatabase* conn : it->second->connections) {
        if (auto* cb = conn->callbacks())
          cb->VersionChange(old_version, blink::IDBDatabaseMetadata::kNoVersion);
      }
      RetireBackend(std::move(it->second));
      Registry().erase(it);
    }
    mojo::AssociatedRemote<IDBFactoryClient> client(std::move(client_pending));
    client->DeleteSuccess(old_version);
  }

 private:
  uint64_t frame_key_ = 0;  // -> the frame's current origin via MbGetFrameOrigin
};

// ---- Persistence: serialize the whole Registry to a flat byte buffer ----------
// Format is private (we own both ends): a length-prefixed binary dump of every
// database's metadata (object stores + indexes + key paths), records (encoded key
// + opaque value bytes), key generators, and secondary-index data. NOTE: a record's
// attached Blob/File payloads (MbRecord::blob_info) are NOT serialized — the bytes live
// in the service-thread MbBlob, which would need an async read here. So blobs round-trip
// IN-SESSION (handles retained) but a SAVED blob record reloads without its blob.

void WU8(std::string* o, uint8_t v) { o->push_back(static_cast<char>(v)); }
void WU32(std::string* o, uint32_t v) {
  for (int i = 0; i < 4; ++i) o->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void WI64(std::string* o, int64_t v) {
  uint64_t u = static_cast<uint64_t>(v);
  for (int i = 0; i < 8; ++i) o->push_back(static_cast<char>((u >> (8 * i)) & 0xff));
}
void WStr(std::string* o, const std::string& s) {
  WU32(o, static_cast<uint32_t>(s.size()));
  o->append(s);
}
void WKeyPath(std::string* o, const blink::IDBKeyPath& kp) {
  switch (kp.GetType()) {
    case blink::mojom::IDBKeyPathType::Null:
      WU8(o, 0);
      break;
    case blink::mojom::IDBKeyPathType::String:
      WU8(o, 1);
      WStr(o, kp.GetString().Utf8());
      break;
    case blink::mojom::IDBKeyPathType::Array: {
      WU8(o, 2);
      const blink::Vector<blink::String>& a = kp.Array();
      WU32(o, a.size());
      for (const blink::String& e : a) WStr(o, e.Utf8());
      break;
    }
  }
}

// Reader with a bounds-checked cursor; any underflow sets ok=false and yields zeros.
struct Reader {
  const std::string& b;
  size_t pos = 0;
  bool ok = true;
  explicit Reader(const std::string& buf) : b(buf) {}
  uint8_t U8() {
    if (pos + 1 > b.size()) { ok = false; return 0; }
    return static_cast<uint8_t>(b[pos++]);
  }
  uint32_t U32() {
    if (pos + 4 > b.size()) { ok = false; return 0; }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(b[pos++])) << (8 * i);
    return v;
  }
  int64_t I64() {
    if (pos + 8 > b.size()) { ok = false; return 0; }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(b[pos++])) << (8 * i);
    return static_cast<int64_t>(v);
  }
  std::string Str() {
    uint32_t n = U32();
    if (!ok || pos + n > b.size()) { ok = false; return std::string(); }
    std::string s = b.substr(pos, n);
    pos += n;
    return s;
  }
  blink::IDBKeyPath KeyPath() {
    uint8_t t = U8();
    if (t == 1)
      return blink::IDBKeyPath(blink::String::FromUtf8(Str()));
    if (t == 2) {
      uint32_t n = U32();
      blink::Vector<blink::String> a;
      for (uint32_t i = 0; i < n && ok; ++i) a.push_back(blink::String::FromUtf8(Str()));
      return blink::IDBKeyPath(a);
    }
    return blink::IDBKeyPath();  // Null
  }
};

constexpr char kIdbMagicV1[] = "MBIDB001";  // records only (no blob payloads)
constexpr char kIdbMagic[] = "MBIDB002";    // + per-record Blob/File bytes

// SerializeRegistry reads each record blob's bytes from this map (keyed by the WebBlobInfo's
// address in the record, stable for the duration of a save). The save gathers the bytes
// asynchronously first (blobs are only readable via an async remote read), then serializes.
// Set/cleared around a single save on the service thread, so a bare pointer is safe.
using BlobByteMap = std::map<const blink::WebBlobInfo*, std::string>;

std::string SerializeRegistry(const BlobByteMap* blob_bytes,
                              const std::string& scope_prefix = std::string()) {
  std::string o;
  o.append(kIdbMagic, 8);
  uint32_t db_count = 0;  // registry keys are "scope\ndbname"; filter by scope
  for (const auto& [name, b] : Registry())
    if (scope_prefix.empty() || name.rfind(scope_prefix, 0) == 0)
      ++db_count;
  WU32(&o, db_count);
  for (const auto& [name, b] : Registry()) {
    if (!scope_prefix.empty() && name.rfind(scope_prefix, 0) != 0)
      continue;
    const blink::IDBDatabaseMetadata& md = b->metadata;
    WStr(&o, name);
    WI64(&o, md.version);
    WI64(&o, md.max_object_store_id);
    WU32(&o, md.object_stores.size());
    for (const auto& [sid, os] : md.object_stores) {
      WI64(&o, sid);
      WStr(&o, os->name.Utf8());
      WKeyPath(&o, os->key_path);
      WU8(&o, os->auto_increment ? 1 : 0);
      WI64(&o, os->max_index_id);
      // Secondary index metadata (id, name, key path, unique, multiEntry).
      WU32(&o, os->indexes.size());
      for (const auto& [iid, idx] : os->indexes) {
        WI64(&o, iid);
        WStr(&o, idx->name.Utf8());
        WKeyPath(&o, idx->key_path);
        WU8(&o, idx->unique ? 1 : 0);
        WU8(&o, idx->multi_entry ? 1 : 0);
      }
    }
    // Records.
    WU32(&o, b->data.size());
    for (const auto& [sid, recs] : b->data) {
      WI64(&o, sid);
      WU32(&o, recs.size());
      for (const auto& [ekey, rec] : recs) {
        WStr(&o, ekey);
        WStr(&o, rec.bytes);
        // Attached Blob/File payloads (v2). The SSV bytes reference these by INDEX, so we
        // persist them in order; the bytes were gathered (async) into `blob_bytes` before
        // this serialize. On load each is re-minted as a fresh inline blob.
        WU32(&o, rec.blob_info.size());
        for (const blink::WebBlobInfo& bi : rec.blob_info) {
          WU8(&o, bi.IsFile() ? 1 : 0);
          WStr(&o, bi.GetType().Utf8());
          WStr(&o, bi.IsFile() ? bi.FileName().Utf8() : std::string());
          std::optional<base::Time> lm = bi.LastModified();
          WI64(&o, lm ? lm->ToDeltaSinceWindowsEpoch().InMicroseconds() : -1);
          std::string bb;
          if (blob_bytes) {
            auto it = blob_bytes->find(&bi);
            if (it != blob_bytes->end())
              bb = it->second;
          }
          WStr(&o, bb);
        }
      }
    }
    // Key generators.
    WU32(&o, b->key_generator.size());
    for (const auto& [sid, next] : b->key_generator) {
      WI64(&o, sid);
      WI64(&o, next);
    }
    // Secondary index DATA: store -> index -> indexKey -> {primaryKeys}.
    WU32(&o, b->index_data.size());
    for (const auto& [sid, idxmap] : b->index_data) {
      WI64(&o, sid);
      WU32(&o, idxmap.size());
      for (const auto& [iid, keymap] : idxmap) {
        WI64(&o, iid);
        WU32(&o, keymap.size());
        for (const auto& [ikey, pkeys] : keymap) {
          WStr(&o, ikey);
          WU32(&o, pkeys.size());
          for (const auto& pk : pkeys)
            WStr(&o, pk);
        }
      }
    }
  }
  return o;
}

bool DeserializeRegistry(const std::string& buf, bool merge = false) {
  Reader r(buf);
  const bool v2 = buf.size() >= 8 && buf.compare(0, 8, kIdbMagic, 8) == 0;
  const bool v1 = buf.size() >= 8 && buf.compare(0, 8, kIdbMagicV1, 8) == 0;
  if (!v1 && !v2)
    return false;
  r.pos = 8;
  uint32_t db_count = r.U32();
  std::map<std::string, std::unique_ptr<MbIDBBackend>> loaded;
  for (uint32_t d = 0; d < db_count && r.ok; ++d) {
    auto b = std::make_unique<MbIDBBackend>();
    std::string key = r.Str();  // the Registry key: "origin\nname" (composite)
    // Recover the page-visible db name from the composite key (the part after the
    // origin separator). An OLD save (key == bare name, no '\n') loads unscoped.
    std::string real_name = key;
    if (auto nl = key.find('\n'); nl != std::string::npos)
      real_name = key.substr(nl + 1);
    b->metadata.name = blink::String::FromUtf8(real_name);
    b->metadata.version = r.I64();
    b->metadata.max_object_store_id = r.I64();
    uint32_t store_count = r.U32();
    for (uint32_t s = 0; s < store_count && r.ok; ++s) {
      int64_t sid = r.I64();
      auto os = blink::IDBObjectStoreMetadata::Create();
      os->id = sid;
      os->name = blink::String::FromUtf8(r.Str());
      os->key_path = r.KeyPath();
      os->auto_increment = r.U8() != 0;
      os->max_index_id = r.I64();
      // Secondary index metadata — reconstruct IDBIndexMetadata (linkable via the
      // 0005-export-idb-index-metadata patch).
      uint32_t index_count = r.U32();
      for (uint32_t ix = 0; ix < index_count && r.ok; ++ix) {
        auto idx = blink::IDBIndexMetadata::Create();
        idx->id = r.I64();
        idx->name = blink::String::FromUtf8(r.Str());
        idx->key_path = r.KeyPath();
        idx->unique = r.U8() != 0;
        idx->multi_entry = r.U8() != 0;
        os->indexes.Set(idx->id, idx);
      }
      b->metadata.object_stores.Set(sid, os);
    }
    uint32_t data_store_count = r.U32();
    for (uint32_t s = 0; s < data_store_count && r.ok; ++s) {
      int64_t sid = r.I64();
      uint32_t rec_count = r.U32();
      auto& recs = b->data[sid];
      for (uint32_t k = 0; k < rec_count && r.ok; ++k) {
        std::string ekey = r.Str();
        std::string bytes = r.Str();
        blink::Vector<blink::WebBlobInfo> blob_info;
        if (v2) {
          uint32_t bcount = r.U32();
          for (uint32_t bi = 0; bi < bcount && r.ok; ++bi) {
            bool is_file = r.U8() != 0;
            std::string type = r.Str();
            std::string fname = r.Str();
            int64_t lm_us = r.I64();
            std::string blob_bytes = r.Str();
            // Re-mint a fresh in-process blob serving the persisted bytes, and rebuild the
            // WebBlobInfo (UUID is fresh — the SSV references blobs by index, not UUID).
            scoped_refptr<blink::BlobDataHandle> handle =
                MbCreateInlineBlob(blob_bytes, blink::String::FromUtf8(type));
            if (is_file) {
              std::optional<base::Time> lm;
              if (lm_us >= 0)
                lm = base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(lm_us));
              blob_info.emplace_back(handle, blink::String::FromUtf8(fname),
                                     blink::String::FromUtf8(type), lm,
                                     blob_bytes.size());
            } else {
              blob_info.emplace_back(handle, blink::String::FromUtf8(type),
                                     blob_bytes.size());
            }
          }
        }
        recs[ekey] =
            MbRecord{DecodeKey(ekey), std::move(bytes), std::move(blob_info)};
      }
    }
    uint32_t keygen_count = r.U32();
    for (uint32_t s = 0; s < keygen_count && r.ok; ++s) {
      int64_t sid = r.I64();
      b->key_generator[sid] = r.I64();
    }
    // Secondary index DATA.
    uint32_t idxdata_count = r.U32();
    for (uint32_t s = 0; s < idxdata_count && r.ok; ++s) {
      int64_t sid = r.I64();
      uint32_t idx_count = r.U32();
      auto& idxmap = b->index_data[sid];
      for (uint32_t ix = 0; ix < idx_count && r.ok; ++ix) {
        int64_t iid = r.I64();
        uint32_t key_count = r.U32();
        auto& keymap = idxmap[iid];
        for (uint32_t k = 0; k < key_count && r.ok; ++k) {
          std::string ikey = r.Str();
          uint32_t pk_count = r.U32();
          auto& pkeys = keymap[ikey];
          for (uint32_t p = 0; p < pk_count && r.ok; ++p)
            pkeys.insert(r.Str());
        }
      }
    }
    if (r.ok)
      loaded[key] = std::move(b);
  }
  if (!r.ok)
    return false;
  // Commit: replace the live registry with the restored set. RETIRE (don't free)
  // the backends being replaced — a page may still hold a live IDB handle whose raw
  // backend_ points into one of them (freeing here was a use-after-free).
  if (merge) {
    // Retire ONLY the backends actually replaced (same key). Retiring every
    // live backend here left the untouched registry slots holding moved-out
    // (null) unique_ptrs — the next whole-registry walk (a session flush's
    // SerializeWithBlobsOnService) crashed dereferencing them.
    for (auto& [k, v] : loaded) {
      auto it = Registry().find(k);
      if (it != Registry().end())
        RetireBackend(std::move(it->second));
      Registry()[k] = std::move(v);  // same-key: file wins
    }
  } else {
    for (auto& kv : Registry())
      RetireBackend(std::move(kv.second));
    Registry() = std::move(loaded);
  }
  return true;
}

}  // namespace

void BindIDBFactory(
    mojo::PendingReceiver<blink::mojom::blink::IDBFactory> receiver,
    uint64_t frame_key) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbIDBFactory>(frame_key),
                              std::move(receiver));
}

namespace {
// Run `task` on the IDB service thread (where the Registry + its backends' mojo
// endpoints live) and block until it finishes. The Registry must only be touched
// there — replacing it on the main thread would destroy service-thread-bound
// AssociatedRemotes off-sequence (a mojo sequence-checker FATAL).
void RunOnServiceSync(base::OnceClosure task) {
  scoped_refptr<base::SingleThreadTaskRunner> runner =
      MbRuntime::ServiceTaskRunner();
  if (!runner || runner->RunsTasksInCurrentSequence()) {
    std::move(task).Run();  // pre-init or already on the service thread
    return;
  }
  base::WaitableEvent done;
  runner->PostTask(FROM_HERE,
                   base::BindOnce(
                       [](base::OnceClosure t, base::WaitableEvent* e) {
                         std::move(t).Run();
                         e->Signal();
                       },
                       std::move(task), &done));
  done.Wait();
}

// Save on the service thread. Blob bytes are only readable via an async remote read, so we
// FIRST read every record blob (concurrently), gather the bytes keyed by the WebBlobInfo's
// stable address, THEN serialize (which reads that map) and signal the waiting main thread.
// Runs entirely on the service thread; the main thread is blocked on `done`, but the service
// thread stays free to drive the async reads — no deadlock. Records are immutable for the
// duration (no IDB op runs while the caller blocks), so the &bi pointers stay valid.
void SerializeWithBlobsOnService(std::string* out, base::WaitableEvent* done,
                                 const std::string& scope_prefix = std::string()) {
  auto byte_map = std::make_unique<BlobByteMap>();
  std::vector<const blink::WebBlobInfo*> blobs;
  for (const auto& [name, b] : Registry())
    for (const auto& [sid, recs] : b->data)
      for (const auto& [ekey, rec] : recs)
        for (const blink::WebBlobInfo& bi : rec.blob_info)
          blobs.push_back(&bi);
  if (blobs.empty()) {
    *out = SerializeRegistry(byte_map.get(), scope_prefix);
    done->Signal();
    return;
  }
  BlobByteMap* raw_map = byte_map.get();
  base::RepeatingClosure barrier = base::BarrierClosure(
      blobs.size(),
      base::BindOnce(
          [](std::string* out, base::WaitableEvent* done,
             std::unique_ptr<BlobByteMap> map, const std::string& prefix) {
            *out = SerializeRegistry(map.get(), prefix);
            done->Signal();
          },
          out, done, std::move(byte_map), scope_prefix));
  for (const blink::WebBlobInfo* bi : blobs) {
    mojo::PendingRemote<blink::mojom::blink::Blob> remote = bi->CloneBlobRemote();
    MbReadBlobRemoteBytes(
        std::move(remote),
        base::BindOnce(
            [](BlobByteMap* map, const blink::WebBlobInfo* key,
               base::RepeatingClosure barrier, std::vector<uint8_t> bytes) {
              (*map)[key] = std::string(bytes.begin(), bytes.end());
              barrier.Run();
            },
            raw_map, bi, barrier));
  }
}
}  // namespace

bool MbSaveIndexedDBScoped(const std::string& path,
                           const std::string& scope_prefix) {
  std::string blob;
  scoped_refptr<base::SingleThreadTaskRunner> runner =
      MbRuntime::ServiceTaskRunner();
  if (!runner || runner->RunsTasksInCurrentSequence()) {
    blob = SerializeRegistry(nullptr, scope_prefix);
  } else {
    base::WaitableEvent done;
    runner->PostTask(FROM_HERE,
                     base::BindOnce(&SerializeWithBlobsOnService, &blob, &done,
                                    scope_prefix));
    done.Wait();
  }
  return base::WriteFile(base::FilePath(path), blob);
}

bool MbLoadIndexedDBMerge(const std::string& path) {
  std::string blob;
  if (!base::ReadFileToString(base::FilePath(path), &blob))
    return false;
  bool ok = false;
  RunOnServiceSync(base::BindOnce(
      [](const std::string& b, bool* out) {
        *out = DeserializeRegistry(b, /*merge=*/true);
      },
      blob, &ok));
  return ok;
}

void MbClearIndexedDBScoped(const std::string& scope_prefix) {
  RunOnServiceSync(base::BindOnce(
      [](const std::string& prefix) {
        auto& reg = Registry();
        for (auto it = reg.begin(); it != reg.end();) {
          if (it->first.rfind(prefix, 0) == 0)
            it = reg.erase(it);
          else
            ++it;
        }
      },
      scope_prefix));
}

bool MbSaveIndexedDB(const std::string& path) {
  std::string blob;
  scoped_refptr<base::SingleThreadTaskRunner> runner =
      MbRuntime::ServiceTaskRunner();
  if (!runner || runner->RunsTasksInCurrentSequence()) {
    // Pre-init or already on the service thread: we can't block on the async blob reads
    // here, so fall back to a record-only serialize (blob payloads empty).
    blob = SerializeRegistry(nullptr);
  } else {
    base::WaitableEvent done;
    runner->PostTask(
        FROM_HERE, base::BindOnce(&SerializeWithBlobsOnService, &blob, &done,
                                  std::string()));
    done.Wait();
  }
  return base::WriteFile(base::FilePath(path), blob);
}

bool MbLoadIndexedDB(const std::string& path) {
  std::string blob;
  if (!base::ReadFileToString(base::FilePath(path), &blob))
    return false;
  bool ok = false;
  RunOnServiceSync(base::BindOnce(
      [](const std::string& b, bool* out) { *out = DeserializeRegistry(b); },
      blob, &ok));
  return ok;
}

}  // namespace mb
