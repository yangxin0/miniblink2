#include "miniblink_host/frame/mb_indexeddb.h"

#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <utility>

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
  // The active connection's database callbacks (transaction completion is reported here).
  mojo::AssociatedRemote<IDBDatabaseCallbacks> db_callbacks;
};

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

std::string ValueBytes(const blink::IDBValue* v) {
  base::span<const uint8_t> d = v->Data();
  return std::string(reinterpret_cast<const char*>(d.data()), d.size());
}

blink::IDBKeyPath StoreKeyPath(MbIDBBackend* b, int64_t store_id) {
  auto it = b->metadata.object_stores.find(store_id);
  return it != b->metadata.object_stores.end() ? it->value->key_path
                                               : blink::IDBKeyPath();
}

// Build the IDBReturnValue blink expects (value bytes + primary key + the store's key path
// — the path MUST match the store's or idb_request.cc DCHECKs).
m::IDBReturnValuePtr BuildReturnValue(const MbRecord& rec,
                                      const blink::IDBKeyPath& key_path) {
  auto rv = m::IDBReturnValue::New();
  rv->value = std::make_unique<blink::IDBValue>();
  rv->value->SetData(mojo_base::BigBuffer(base::as_byte_span(rec.bytes)));
  rv->primary_key = blink::IDBKey::Clone(rec.key);
  rv->key_path = key_path;
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
  void Get(int64_t, int64_t object_store_id, int64_t, m::IDBKeyRangePtr key_range,
           bool key_only, GetCallback callback) override {
    const blink::IDBKey* k = key_range ? key_range->lower.get() : nullptr;
    std::string ekey = EncodeKey(k);
    auto store_it = backend_->data.find(object_store_id);
    if (ekey.empty() || store_it == backend_->data.end() ||
        !store_it->second.count(ekey)) {
      std::move(callback).Run(m::IDBDatabaseGetResult::NewEmpty(true));
      return;
    }
    const MbRecord& rec = store_it->second.at(ekey);
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
  void GetAll(int64_t, int64_t object_store_id, int64_t,
              m::IDBKeyRangePtr key_range, m::IDBGetAllResultType result_type,
              uint32_t max_count, m::IDBCursorDirection direction,
              GetAllCallback callback) override {
    blink::Vector<m::IDBRecordPtr> records;
    auto store_it = backend_->data.find(object_store_id);
    if (store_it != backend_->data.end()) {
      const auto& store = store_it->second;
      // Encoded-key bounds for the range (order-preserving encoding makes this exact).
      std::string lo = key_range ? EncodeKey(key_range->lower.get()) : std::string();
      std::string hi = key_range ? EncodeKey(key_range->upper.get()) : std::string();
      const blink::IDBKeyPath kp = StoreKeyPath(backend_, object_store_id);
      const bool keys_only = result_type == m::IDBGetAllResultType::Keys;
      const bool reverse = direction == m::IDBCursorDirection::Prev ||
                           direction == m::IDBCursorDirection::PrevNoDuplicate;
      const uint32_t limit = max_count == 0 ? UINT32_MAX : max_count;

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
      auto emit = [&](const std::string& ek, const MbRecord& rec) {
        auto record = m::IDBRecord::New();
        // Values-only getAll() must NOT carry a primary key (blink CHECKs this); keys are
        // set only for getAllKeys()/getAllRecords().
        if (result_type != m::IDBGetAllResultType::Values)
          record->primary_key = blink::IDBKey::Clone(rec.key);
        if (!keys_only)
          record->return_value = BuildReturnValue(rec, kp);
        records.push_back(std::move(record));
      };
      if (reverse) {
        for (auto it = store.rbegin();
             it != store.rend() && records.size() < limit; ++it)
          if (in_range(it->first))
            emit(it->first, it->second);
      } else {
        for (auto it = store.begin();
             it != store.end() && records.size() < limit; ++it)
          if (in_range(it->first))
            emit(it->first, it->second);
      }
    }
    std::move(callback).Run(std::move(records), mojo::NullAssociatedReceiver());
  }
  void OpenCursor(int64_t, int64_t, int64_t, m::IDBKeyRangePtr,
                  m::IDBCursorDirection, bool, m::IDBTaskType,
                  OpenCursorCallback callback) override {
    std::move(callback).Run(nullptr);
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
      if (ekey.empty())
        it->second.clear();
      else
        it->second.erase(ekey);
    }
    std::move(callback).Run(/*success=*/true);
  }
  void GetKeyGeneratorCurrentNumber(
      int64_t, int64_t,
      GetKeyGeneratorCurrentNumberCallback callback) override {
    std::move(callback).Run(0, nullptr);
  }
  void Clear(int64_t, int64_t object_store_id,
             ClearCallback callback) override {
    auto it = backend_->data.find(object_store_id);
    if (it != backend_->data.end())
      it->second.clear();
    std::move(callback).Run(/*success=*/true);
  }
  void CreateIndex(int64_t, int64_t,
                   const scoped_refptr<blink::IDBIndexMetadata>&) override {}
  void DeleteIndex(int64_t, int64_t, int64_t) override {}
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
           blink::Vector<blink::IDBIndexKeys>,
           PutCallback callback) override {
    std::string ekey = EncodeKey(key.get());
    if (ekey.empty()) {
      std::move(callback).Run(m::IDBTransactionPutResult::NewErrorResult(
          m::IDBError::New(m::IDBException::kDataError,
                           blink::String("unsupported key type"))));
      return;
    }
    backend_->data[object_store_id][ekey] =
        MbRecord{blink::IDBKey::Clone(key), ValueBytes(value.get())};
    std::move(callback).Run(
        m::IDBTransactionPutResult::NewKey(blink::IDBKey::Clone(key)));
  }
  void SetIndexKeys(int64_t,
                    std::unique_ptr<blink::IDBKey>,
                    blink::IDBIndexKeys) override {}
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
