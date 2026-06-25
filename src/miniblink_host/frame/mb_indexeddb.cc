#include "miniblink_host/frame/mb_indexeddb.h"

#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/renderer/modules/indexeddb/idb_metadata.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace mb {
namespace {

namespace m = blink::mojom::blink;
using m::IDBDatabaseCallbacks;
using m::IDBFactoryClient;
using m::IDBTransaction;

// In-memory backing store for one database name: its current version + schema (object
// store metadata). Process-wide so reopen sees the same database. (Step 2 adds the records.)
struct MbIDBBackend {
  blink::IDBDatabaseMetadata metadata;  // name/version/object_stores
};

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
class MbIDBDatabase : public m::IDBDatabase {
 public:
  explicit MbIDBDatabase(MbIDBBackend*) {}

  void RenameObjectStore(int64_t, int64_t, const blink::String&) override {}
  void CreateTransaction(
      mojo::PendingAssociatedReceiver<IDBTransaction>,
      int64_t,
      const blink::Vector<int64_t>&,
      m::IDBTransactionMode,
      m::IDBTransactionDurability) override {}
  void VersionChangeIgnored() override {}
  void Get(int64_t, int64_t, int64_t, m::IDBKeyRangePtr, bool,
           GetCallback callback) override {
    std::move(callback).Run(m::IDBDatabaseGetResult::NewEmpty(true));
  }
  void GetAll(int64_t, int64_t, int64_t, m::IDBKeyRangePtr,
              m::IDBGetAllResultType, uint32_t, m::IDBCursorDirection,
              GetAllCallback callback) override {
    std::move(callback).Run({}, mojo::NullAssociatedReceiver());
  }
  void OpenCursor(int64_t, int64_t, int64_t, m::IDBKeyRangePtr,
                  m::IDBCursorDirection, bool, m::IDBTaskType,
                  OpenCursorCallback callback) override {
    std::move(callback).Run(nullptr);
  }
  void Count(int64_t, int64_t, int64_t, m::IDBKeyRangePtr,
             CountCallback callback) override {
    std::move(callback).Run(/*success=*/false, 0);
  }
  void DeleteRange(int64_t, int64_t, m::IDBKeyRangePtr,
                   DeleteRangeCallback callback) override {
    std::move(callback).Run(/*success=*/true);
  }
  void GetKeyGeneratorCurrentNumber(
      int64_t, int64_t,
      GetKeyGeneratorCurrentNumberCallback callback) override {
    std::move(callback).Run(0, nullptr);
  }
  void Clear(int64_t, int64_t, ClearCallback callback) override {
    std::move(callback).Run(/*success=*/true);
  }
  void CreateIndex(int64_t, int64_t,
                   const scoped_refptr<blink::IDBIndexMetadata>&) override {}
  void DeleteIndex(int64_t, int64_t, int64_t) override {}
  void RenameIndex(int64_t, int64_t, int64_t, const blink::String&) override {}
  void Abort(int64_t) override {}
  void DidBecomeInactive() override {}
  void UpdatePriority(int32_t) override {}
};

// A transaction. For the version-change transaction created during open, CreateObjectStore
// records schema and Commit fires the open success. (Read/write data ops: step 2.)
class MbIDBTransactionImpl : public IDBTransaction {
 public:
  MbIDBTransactionImpl(MbIDBBackend* backend,
                       int64_t txn_id,
                       mojo::AssociatedRemote<IDBFactoryClient> factory_client,
                       mojo::AssociatedRemote<IDBDatabaseCallbacks> db_callbacks)
      : backend_(backend),
        txn_id_(txn_id),
        factory_client_(std::move(factory_client)),
        db_callbacks_(std::move(db_callbacks)) {}

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
  void Put(int64_t,
           std::unique_ptr<blink::IDBValue>,
           std::unique_ptr<blink::IDBKey>,
           m::IDBPutMode,
           blink::Vector<blink::IDBIndexKeys>,
           PutCallback callback) override {
    // Step 2: persist. For now report a clean no-op error so the request settles.
    std::move(callback).Run(m::IDBTransactionPutResult::NewErrorResult(
        m::IDBError::New(m::IDBException::kUnknownError,
                         blink::String("put not implemented (step 1)"))));
  }
  void SetIndexKeys(int64_t,
                    std::unique_ptr<blink::IDBKey>,
                    blink::IDBIndexKeys) override {}
  void SetIndexKeysDone() override {}
  void Commit(int64_t /*num_errors_handled*/) override {
    if (db_callbacks_)
      db_callbacks_->Complete(txn_id_);
    if (factory_client_) {
      factory_client_->OpenSuccess(mojo::NullAssociatedRemote(),
                                   backend_->metadata);
    }
  }

 private:
  MbIDBBackend* backend_;
  int64_t txn_id_;
  mojo::AssociatedRemote<IDBFactoryClient> factory_client_;
  mojo::AssociatedRemote<IDBDatabaseCallbacks> db_callbacks_;
};

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
    mojo::AssociatedRemote<IDBDatabaseCallbacks> db_callbacks(
        std::move(db_cb_pending));

    // The database handle blink will drive.
    mojo::PendingAssociatedRemote<m::IDBDatabase> db_remote;
    auto db_receiver = db_remote.InitWithNewEndpointAndPassReceiver();
    mojo::MakeSelfOwnedAssociatedReceiver(
        std::make_unique<MbIDBDatabase>(backend), std::move(db_receiver));

    const int64_t current = CurrentVersion(backend);
    if (version > current) {
      backend->metadata.version = version;
      auto txn = std::make_unique<MbIDBTransactionImpl>(
          backend, transaction_id, std::move(client),
          std::move(db_callbacks));
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
