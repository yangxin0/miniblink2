#include "miniblink_host/frame/mb_opfs.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <algorithm>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "miniblink_host/blob/mb_blob_registry.h"
#include "miniblink_host/frame/mb_frame_origin.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "mojo/public/cpp/system/data_pipe_utils.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_access_handle_host.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_cloud_identifier.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_file_delegate_host.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_directory_handle.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_error.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_file_handle.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_file_writer.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_manager.mojom-blink.h"
#include "third_party/blink/public/mojom/permissions/permission_status.mojom-blink.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace mb {
namespace {

namespace m = blink::mojom::blink;

// An in-memory OPFS node: a directory (named children) or a file (raw bytes). Roots
// persist across getDirectory() calls within a run (files created earlier are seen
// again), keyed by a storage scope (origin, or origin+bucket) so OPFS is ISOLATED
// per origin — the same per-origin requirement as IndexedDB; a process-wide root
// would let different origins read/write each other's private files.
// Children and bound handles hold the node via shared_ptr so a node that is removed from the
// tree (RemoveEntry drops the parent's map entry) stays alive while any open handle/writer/
// sync-access host references it — no dangling FsNode* / use-after-free on a removed-but-open
// handle. Per-file lock state lives on the node so all handles for the same file contend.
struct FsNode {
  bool is_dir = true;
  std::map<std::string, std::shared_ptr<FsNode>> children;  // when is_dir
  std::string bytes;                                        // when !is_dir
  // Exclusive-access bookkeeping (createSyncAccessHandle / createWritable locks).
  int lock_shared = 0;          // count of shared (read-only / siloed / unsafe) locks
  bool lock_exclusive = false;  // a single exclusive (readwrite / exclusive) lock
};

// Acquire/release a per-file lock. Exclusive conflicts with any other lock; shared only with
// an exclusive one. Returns false (caller maps to NoModificationAllowedError) on conflict.
bool AcquireLock(FsNode* n, bool exclusive) {
  if (exclusive) {
    if (n->lock_exclusive || n->lock_shared > 0)
      return false;
    n->lock_exclusive = true;
  } else {
    if (n->lock_exclusive)
      return false;
    ++n->lock_shared;
  }
  return true;
}
void ReleaseLock(FsNode* n, bool exclusive) {
  if (exclusive)
    n->lock_exclusive = false;
  else if (n->lock_shared > 0)
    --n->lock_shared;
}

// Process-wide scope -> root tree (origin / origin+bucket isolated). Shared by OpfsRoot
// and the persistence (save/load) below.
std::map<std::string, std::shared_ptr<FsNode>>& OpfsRoots() {
  static auto* roots =
      new std::map<std::string, std::shared_ptr<FsNode>>();  // scope -> root
  return *roots;
}

std::shared_ptr<FsNode> OpfsRoot(const std::string& scope) {
  auto& roots = OpfsRoots();
  auto it = roots.find(scope);
  if (it == roots.end())
    it = roots.emplace(scope, std::make_shared<FsNode>()).first;
  return it->second;
}

m::FileSystemAccessErrorPtr Ok() {
  return m::FileSystemAccessError::New(m::FileSystemAccessStatus::kOk,
                                       base::File::FILE_OK, blink::String::FromUtf8(""));
}
// kFileError carries a base::File::Error that blink maps to the right DOMException
// (FILE_ERROR_NOT_FOUND -> NotFoundError, etc.).
m::FileSystemAccessErrorPtr FileError(base::File::Error e, const char* msg) {
  return m::FileSystemAccessError::New(m::FileSystemAccessStatus::kFileError, e,
                                       blink::String::FromUtf8(msg));
}
m::FileSystemAccessErrorPtr NotSupported(const char* msg) {
  return m::FileSystemAccessError::New(
      m::FileSystemAccessStatus::kNotSupportedError,
      base::File::FILE_ERROR_FAILED, blink::String::FromUtf8(msg));
}

mojo::PendingRemote<m::FileSystemAccessFileHandle> BindFile(
    std::shared_ptr<FsNode> node);
mojo::PendingRemote<m::FileSystemAccessDirectoryHandle> BindDir(
    std::shared_ptr<FsNode> node);

// ----------------------------- file writer -----------------------------
// A writable session over one file. Writes accumulate into a working buffer (a copy of the
// file's bytes when keep_existing_data); Close() commits the buffer back to the node, Abort()
// discards it. Each Write drains its data pipe, then splices the bytes in at the given offset.
class MbFsFileWriter : public m::FileSystemAccessFileWriter {
 public:
  MbFsFileWriter(std::shared_ptr<FsNode> node, bool keep_existing_data,
                 bool locked_exclusive)
      : node_(std::move(node)),
        buffer_(keep_existing_data ? node_->bytes : std::string()),
        locked_exclusive_(locked_exclusive) {}
  ~MbFsFileWriter() override { ReleaseLock(node_.get(), locked_exclusive_); }

  void Write(uint64_t offset, mojo::ScopedDataPipeConsumerHandle stream,
             WriteCallback cb) override {
    ops_.push_back(std::make_unique<WriteOp>(
        this, static_cast<size_t>(offset), std::move(stream), std::move(cb)));
  }
  void Truncate(uint64_t length, TruncateCallback cb) override {
    buffer_.resize(static_cast<size_t>(length), '\0');
    std::move(cb).Run(Ok());
  }
  void Close(CloseCallback cb) override {
    node_->bytes = buffer_;  // commit the session atomically
    std::move(cb).Run(Ok());
  }
  void Abort(AbortCallback cb) override {
    std::move(cb).Run(Ok());  // drop the uncommitted buffer
  }

 private:
  // Drains one Write's data pipe, then asks the writer to splice + answer the callback.
  class WriteOp : public mojo::DataPipeDrainer::Client {
   public:
    WriteOp(MbFsFileWriter* writer, size_t offset,
            mojo::ScopedDataPipeConsumerHandle stream, WriteCallback cb)
        : writer_(writer),
          offset_(offset),
          cb_(std::move(cb)),
          drainer_(this, std::move(stream)) {}
    void OnDataAvailable(base::span<const uint8_t> data) override {
      data_.append(reinterpret_cast<const char*>(data.data()), data.size());
    }
    void OnDataComplete() override {
      // Don't delete this op here (its drainer is mid-callback) — the writer keeps it
      // until the session ends.
      writer_->ApplyWrite(offset_, data_, std::move(cb_));
    }

   private:
    MbFsFileWriter* writer_;
    size_t offset_;
    WriteCallback cb_;
    std::string data_;
    mojo::DataPipeDrainer drainer_;
  };

  void ApplyWrite(size_t offset, const std::string& data, WriteCallback cb) {
    if (offset > buffer_.size())
      buffer_.resize(offset, '\0');  // a gap past EOF is zero-filled
    for (size_t i = 0; i < data.size(); ++i) {
      const size_t pos = offset + i;
      if (pos < buffer_.size())
        buffer_[pos] = data[i];
      else
        buffer_.push_back(data[i]);
    }
    std::move(cb).Run(Ok(), data.size());
  }

  std::shared_ptr<FsNode> node_;
  std::string buffer_;
  std::vector<std::unique_ptr<WriteOp>> ops_;
  bool locked_exclusive_ = false;
};

// ------------------------- sync access handle --------------------------
// The in-memory ("incognito") delegate behind createSyncAccessHandle(). A worker reads/writes
// the file synchronously via these [Sync] RPCs; they operate directly on the node's bytes.
class MbFsFileDelegateHost : public m::FileSystemAccessFileDelegateHost {
 public:
  explicit MbFsFileDelegateHost(std::shared_ptr<FsNode> node)
      : node_(std::move(node)) {}

  void Read(int64_t offset, int32_t bytes_to_read, ReadCallback cb) override {
    const std::string& b = node_->bytes;
    if (offset < 0 || bytes_to_read < 0) {
      std::move(cb).Run(std::nullopt, base::File::FILE_ERROR_FAILED, 0);
      return;
    }
    const size_t off = static_cast<size_t>(offset);
    const size_t n = off >= b.size()
                         ? 0
                         : std::min(static_cast<size_t>(bytes_to_read),
                                    b.size() - off);
    std::string slice = b.substr(off, n);
    std::move(cb).Run(mojo_base::BigBuffer(base::as_byte_span(slice)),
                      base::File::FILE_OK, static_cast<int32_t>(n));
  }
  // blink feeds `data` from another thread and closes it, so a blocking drain can't deadlock.
  void Write(int64_t offset, mojo::ScopedDataPipeConsumerHandle data,
             WriteCallback cb) override {
    std::string incoming;
    mojo::BlockingCopyToString(std::move(data), &incoming);
    std::string& b = node_->bytes;
    const size_t off = static_cast<size_t>(offset < 0 ? 0 : offset);
    if (off > b.size())
      b.resize(off, '\0');
    for (size_t i = 0; i < incoming.size(); ++i) {
      const size_t p = off + i;
      if (p < b.size())
        b[p] = incoming[i];
      else
        b.push_back(incoming[i]);
    }
    std::move(cb).Run(base::File::FILE_OK,
                      static_cast<int32_t>(incoming.size()));
  }
  void GetLength(GetLengthCallback cb) override {
    std::move(cb).Run(base::File::FILE_OK,
                      static_cast<int64_t>(node_->bytes.size()));
  }
  void SetLength(int64_t length, SetLengthCallback cb) override {
    node_->bytes.resize(static_cast<size_t>(length < 0 ? 0 : length), '\0');
    std::move(cb).Run(base::File::FILE_OK);
  }

 private:
  std::shared_ptr<FsNode> node_;
};

// The browser-side handle whose Close() releases the file's (exclusive/shared) access lock.
// Also releases on pipe disconnect (destructor) so an aborted handle doesn't leak the lock.
class MbFsAccessHandleHost : public m::FileSystemAccessAccessHandleHost {
 public:
  MbFsAccessHandleHost(std::shared_ptr<FsNode> node, bool locked_exclusive)
      : node_(std::move(node)), locked_exclusive_(locked_exclusive) {}
  ~MbFsAccessHandleHost() override { Release(); }
  void Close(CloseCallback cb) override {
    Release();
    std::move(cb).Run();
  }

 private:
  void Release() {
    if (held_) {
      ReleaseLock(node_.get(), locked_exclusive_);
      held_ = false;
    }
  }
  std::shared_ptr<FsNode> node_;
  bool locked_exclusive_ = false;
  bool held_ = true;
};

// ----------------------------- file handle -----------------------------
class MbFsFileHandle : public m::FileSystemAccessFileHandle {
 public:
  explicit MbFsFileHandle(std::shared_ptr<FsNode> node)
      : node_(std::move(node)) {}

  void GetPermissionStatus(m::FileSystemAccessPermissionMode,
                           GetPermissionStatusCallback cb) override {
    std::move(cb).Run(m::PermissionStatus::GRANTED);
  }
  void RequestPermission(m::FileSystemAccessPermissionMode,
                         RequestPermissionCallback cb) override {
    std::move(cb).Run(Ok(), m::PermissionStatus::GRANTED);
  }
  // getFile(): hand back a Blob serving the file's current bytes (+ size metadata).
  void AsBlob(AsBlobCallback cb) override {
    base::File::Info info;
    info.size = static_cast<int64_t>(node_->bytes.size());
    scoped_refptr<blink::BlobDataHandle> blob =
        MbCreateInlineBlob(node_->bytes, blink::String::FromUtf8(""));
    std::move(cb).Run(Ok(), info, blob);
  }
  // createWritable(): open a writing session over this file.
  void CreateFileWriter(bool keep_existing_data, bool /*auto_close*/,
                        m::FileSystemAccessWritableFileStreamLockMode mode,
                        CreateFileWriterCallback cb) override {
    // Only the "siloed" mode is shared; "exclusive" takes an exclusive lock.
    const bool exclusive =
        mode == m::FileSystemAccessWritableFileStreamLockMode::kExclusive;
    if (!AcquireLock(node_.get(), exclusive)) {
      std::move(cb).Run(
          FileError(base::File::FILE_ERROR_IN_USE, "file is locked"),
          mojo::NullRemote());
      return;
    }
    mojo::PendingRemote<m::FileSystemAccessFileWriter> remote;
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbFsFileWriter>(node_, keep_existing_data, exclusive),
        remote.InitWithNewPipeAndPassReceiver());
    std::move(cb).Run(Ok(), std::move(remote));
  }
  // createSyncAccessHandle() (Worker-only): hand back an in-memory file delegate the worker
  // drives synchronously, plus a host whose Close() releases the (notional) lock.
  void OpenAccessHandle(m::FileSystemAccessAccessHandleLockMode mode,
                        OpenAccessHandleCallback cb) override {
    // Only read-only / readwrite-unsafe are shared; readwrite is exclusive.
    const bool exclusive =
        mode == m::FileSystemAccessAccessHandleLockMode::kReadwrite;
    if (!AcquireLock(node_.get(), exclusive)) {
      std::move(cb).Run(
          FileError(base::File::FILE_ERROR_IN_USE, "file is locked"), nullptr,
          mojo::NullRemote());
      return;
    }
    mojo::PendingRemote<m::FileSystemAccessFileDelegateHost> delegate;
    mojo::MakeSelfOwnedReceiver(std::make_unique<MbFsFileDelegateHost>(node_),
                                delegate.InitWithNewPipeAndPassReceiver());
    mojo::PendingRemote<m::FileSystemAccessAccessHandleHost> host;
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbFsAccessHandleHost>(node_, exclusive),
        host.InitWithNewPipeAndPassReceiver());
    std::move(cb).Run(
        Ok(),
        m::FileSystemAccessAccessHandleFile::NewIncognitoFileDelegate(
            std::move(delegate)),
        std::move(host));
  }
  void Rename(const blink::String&, RenameCallback cb) override {
    std::move(cb).Run(NotSupported("rename not supported"));
  }
  void Move(mojo::PendingRemote<m::FileSystemAccessTransferToken>,
            const blink::String&, MoveCallback cb) override {
    std::move(cb).Run(NotSupported("move not supported"));
  }
  void Remove(RemoveCallback cb) override {
    std::move(cb).Run(NotSupported("remove not supported"));
  }
  void IsSameEntry(mojo::PendingRemote<m::FileSystemAccessTransferToken>,
                   IsSameEntryCallback cb) override {
    std::move(cb).Run(Ok(), false);
  }
  void Transfer(
      mojo::PendingReceiver<m::FileSystemAccessTransferToken>) override {}
  void GetUniqueId(GetUniqueIdCallback cb) override {
    std::move(cb).Run(Ok(), blink::String::FromUtf8(""));
  }
  void GetCloudIdentifiers(GetCloudIdentifiersCallback cb) override {
    std::move(cb).Run(Ok(), {});
  }

 private:
  std::shared_ptr<FsNode>
      node_;  // the file node whose bytes back read (AsBlob) and write sessions
};

// --------------------------- directory handle ---------------------------
class MbFsDirectoryHandle : public m::FileSystemAccessDirectoryHandle {
 public:
  explicit MbFsDirectoryHandle(std::shared_ptr<FsNode> node)
      : node_(std::move(node)) {}

  void GetPermissionStatus(m::FileSystemAccessPermissionMode,
                           GetPermissionStatusCallback cb) override {
    std::move(cb).Run(m::PermissionStatus::GRANTED);
  }
  void RequestPermission(m::FileSystemAccessPermissionMode,
                         RequestPermissionCallback cb) override {
    std::move(cb).Run(Ok(), m::PermissionStatus::GRANTED);
  }
  void GetFile(const blink::String& basename, bool create,
               GetFileCallback cb) override {
    std::string name = basename.Utf8();
    auto it = node_->children.find(name);
    if (it == node_->children.end()) {
      if (!create) {
        std::move(cb).Run(
            FileError(base::File::FILE_ERROR_NOT_FOUND, "file not found"),
            mojo::NullRemote());
        return;
      }
      auto child = std::make_shared<FsNode>();
      child->is_dir = false;
      it = node_->children.emplace(name, std::move(child)).first;
    } else if (it->second->is_dir) {
      std::move(cb).Run(
          FileError(base::File::FILE_ERROR_NOT_A_FILE, "name is a directory"),
          mojo::NullRemote());
      return;
    }
    std::move(cb).Run(Ok(), BindFile(it->second));
  }
  void GetDirectory(const blink::String& basename, bool create,
                    GetDirectoryCallback cb) override {
    std::string name = basename.Utf8();
    auto it = node_->children.find(name);
    if (it == node_->children.end()) {
      if (!create) {
        std::move(cb).Run(
            FileError(base::File::FILE_ERROR_NOT_FOUND, "directory not found"),
            mojo::NullRemote());
        return;
      }
      it = node_->children.emplace(name, std::make_shared<FsNode>())
               .first;  // is_dir = true
    } else if (!it->second->is_dir) {
      std::move(cb).Run(
          FileError(base::File::FILE_ERROR_NOT_A_DIRECTORY, "name is a file"),
          mojo::NullRemote());
      return;
    }
    std::move(cb).Run(Ok(), BindDir(it->second));
  }
  void GetEntries(mojo::PendingRemote<m::FileSystemAccessDirectoryEntriesListener>
                      listener) override {
    mojo::Remote<m::FileSystemAccessDirectoryEntriesListener> l(
        std::move(listener));
    blink::Vector<m::FileSystemAccessEntryPtr> entries;
    for (auto& kv : node_->children) {
      m::FileSystemAccessHandlePtr handle =
          kv.second->is_dir
              ? m::FileSystemAccessHandle::NewDirectory(BindDir(kv.second))
              : m::FileSystemAccessHandle::NewFile(BindFile(kv.second));
      entries.push_back(m::FileSystemAccessEntry::New(
          std::move(handle), blink::String::FromUtf8(kv.first)));
    }
    l->DidReadDirectory(Ok(), std::move(entries), /*has_more_entries=*/false);
    listeners_.push_back(std::move(l));  // keep alive until the message flushes
  }
  void RemoveEntry(const blink::String& basename, bool recurse,
                   RemoveEntryCallback cb) override {
    std::string name = basename.Utf8();
    auto it = node_->children.find(name);
    if (it == node_->children.end()) {
      std::move(cb).Run(
          FileError(base::File::FILE_ERROR_NOT_FOUND, "entry not found"));
      return;
    }
    // A non-empty directory may only be removed recursively (InvalidModificationError
    // otherwise, which FILE_ERROR_NOT_EMPTY maps to).
    if (it->second->is_dir && !recurse && !it->second->children.empty()) {
      std::move(cb).Run(
          FileError(base::File::FILE_ERROR_NOT_EMPTY, "directory not empty"));
      return;
    }
    node_->children.erase(it);
    std::move(cb).Run(Ok());
  }
  void Rename(const blink::String&, RenameCallback cb) override {
    std::move(cb).Run(NotSupported("rename not supported"));
  }
  void Move(mojo::PendingRemote<m::FileSystemAccessTransferToken>,
            const blink::String&, MoveCallback cb) override {
    std::move(cb).Run(NotSupported("move not supported"));
  }
  void Remove(bool, RemoveCallback cb) override {
    std::move(cb).Run(NotSupported("remove not supported"));
  }
  void Resolve(mojo::PendingRemote<m::FileSystemAccessTransferToken>,
               ResolveCallback cb) override {
    std::move(cb).Run(Ok(), std::nullopt);
  }
  void Transfer(
      mojo::PendingReceiver<m::FileSystemAccessTransferToken>) override {}
  void GetUniqueId(GetUniqueIdCallback cb) override {
    std::move(cb).Run(Ok(), blink::String::FromUtf8(""));
  }
  void GetCloudIdentifiers(GetCloudIdentifiersCallback cb) override {
    std::move(cb).Run(Ok(), {});
  }

 private:
  std::shared_ptr<FsNode> node_;
  std::vector<mojo::Remote<m::FileSystemAccessDirectoryEntriesListener>>
      listeners_;
};

mojo::PendingRemote<m::FileSystemAccessFileHandle> BindFile(
    std::shared_ptr<FsNode> node) {
  mojo::PendingRemote<m::FileSystemAccessFileHandle> remote;
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbFsFileHandle>(std::move(node)),
                              remote.InitWithNewPipeAndPassReceiver());
  return remote;
}
mojo::PendingRemote<m::FileSystemAccessDirectoryHandle> BindDir(
    std::shared_ptr<FsNode> node) {
  mojo::PendingRemote<m::FileSystemAccessDirectoryHandle> remote;
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<MbFsDirectoryHandle>(std::move(node)),
      remote.InitWithNewPipeAndPassReceiver());
  return remote;
}

// ------------------------------- manager -------------------------------
class MbFileSystemAccessManager : public m::FileSystemAccessManager {
 public:
  explicit MbFileSystemAccessManager(uint64_t frame_key)
      : frame_key_(frame_key) {}

  void GetSandboxedFileSystem(GetSandboxedFileSystemCallback cb) override {
    std::move(cb).Run(Ok(), BindDir(OpfsRoot(MbGetFrameOrigin(frame_key_))));
  }
  void GetSandboxedFileSystemForDevtools(
      const blink::Vector<blink::String>&,
      GetSandboxedFileSystemForDevtoolsCallback cb) override {
    std::move(cb).Run(Ok(), BindDir(OpfsRoot(MbGetFrameOrigin(frame_key_))));
  }
  void ChooseEntries(m::FilePickerOptionsPtr,
                     ChooseEntriesCallback cb) override {
    std::move(cb).Run(NotSupported("file pickers not supported"), {});
  }
  void GetFileHandleFromToken(
      mojo::PendingRemote<m::FileSystemAccessTransferToken>,
      mojo::PendingReceiver<m::FileSystemAccessFileHandle>) override {}
  void GetDirectoryHandleFromToken(
      mojo::PendingRemote<m::FileSystemAccessTransferToken>,
      mojo::PendingReceiver<m::FileSystemAccessDirectoryHandle>) override {}
  void GetEntryFromDataTransferToken(
      mojo::PendingRemote<m::FileSystemAccessDataTransferToken>,
      GetEntryFromDataTransferTokenCallback cb) override {
    std::move(cb).Run(NotSupported("data transfer not supported"), nullptr);
  }
  void BindObserverHost(
      mojo::PendingReceiver<m::FileSystemAccessObserverHost>) override {}

 private:
  uint64_t frame_key_ = 0;  // -> the frame's origin, scoping the OPFS root
};

}  // namespace

void BindFileSystemAccessManager(
    mojo::PendingReceiver<blink::mojom::blink::FileSystemAccessManager> receiver,
    uint64_t frame_key) {
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<MbFileSystemAccessManager>(frame_key),
      std::move(receiver));
}

mojo::PendingRemote<blink::mojom::blink::FileSystemAccessDirectoryHandle>
MbBindOpfsRootDirectory(const std::string& scope) {
  return BindDir(OpfsRoot(scope));
}

namespace {

// --- OPFS persistence (binary, recursive tree) ---
constexpr char kOpfsMagic[] = "MBOPFS01";

void WLen(std::string* o, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    o->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void WBlob(std::string* o, const std::string& s) {
  WLen(o, static_cast<uint32_t>(s.size()));
  o->append(s);
}
// Serialize one node: is_dir byte; dir -> child count + (name, child)*; file -> bytes.
void WNode(std::string* o, const FsNode* n) {
  o->push_back(n->is_dir ? 1 : 0);
  if (n->is_dir) {
    WLen(o, static_cast<uint32_t>(n->children.size()));
    for (const auto& [name, child] : n->children) {
      WBlob(o, name);
      WNode(o, child.get());
    }
  } else {
    WBlob(o, n->bytes);
  }
}

struct OpfsReader {
  const std::string& buf;
  size_t pos = 0;
  bool ok = true;
  uint32_t Len() {
    if (pos + 4 > buf.size()) { ok = false; return 0; }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v |= static_cast<uint32_t>(static_cast<uint8_t>(buf[pos++])) << (8 * i);
    return v;
  }
  uint8_t Byte() {
    if (pos >= buf.size()) { ok = false; return 0; }
    return static_cast<uint8_t>(buf[pos++]);
  }
  std::string Blob() {
    uint32_t n = Len();
    if (!ok || pos + n > buf.size()) { ok = false; return std::string(); }
    std::string s = buf.substr(pos, n);
    pos += n;
    return s;
  }
};

// Apply a serialized node ONTO a live FsNode (merge: update bytes / recurse into existing
// children, create missing ones; never deletes). Keeps existing FsNode* pointers valid so
// any bound directory/file handles survive a load.
void ReadNodeInto(OpfsReader* r, FsNode* dst) {
  bool is_dir = r->Byte() != 0;
  if (!r->ok)
    return;
  dst->is_dir = is_dir;
  if (is_dir) {
    uint32_t count = r->Len();
    for (uint32_t i = 0; i < count && r->ok; ++i) {
      std::string name = r->Blob();
      if (!r->ok)
        return;
      auto it = dst->children.find(name);
      if (it == dst->children.end())
        it = dst->children.emplace(name, std::make_shared<FsNode>()).first;
      ReadNodeInto(r, it->second.get());
    }
  } else {
    dst->bytes = r->Blob();
  }
}

// Merge a fully validated detached tree onto a live tree. Existing nodes are
// updated in place so any already-bound handles remain valid.
void MergeNodeInto(const FsNode& src, FsNode* dst) {
  dst->is_dir = src.is_dir;
  if (!src.is_dir) {
    dst->bytes = src.bytes;
    return;
  }
  for (const auto& [name, child] : src.children) {
    auto it = dst->children.find(name);
    if (it == dst->children.end())
      it = dst->children.emplace(name, std::make_shared<FsNode>()).first;
    MergeNodeInto(*child, it->second.get());
  }
}

bool ParseAndMergeScopedOpfs(const std::string& buf,
                             const std::string& target_scope_prefix,
                             bool* out_rekeyed) {
  if (out_rekeyed)
    *out_rekeyed = false;
  if (buf.size() < 8 || buf.compare(0, 8, kOpfsMagic, 8) != 0)
    return false;

  // Parse into detached roots first. A corrupt/truncated file must not leave a
  // partial live tree that a later flush could serialize over the source.
  OpfsReader r{buf, 8};
  const uint32_t scope_count = r.Len();
  std::map<std::string, std::shared_ptr<FsNode>> loaded;
  std::string source_scope_prefix;
  bool rekeyed = false;
  for (uint32_t i = 0; i < scope_count && r.ok; ++i) {
    std::string scope = r.Blob();
    if (!r.ok)
      return false;
    const size_t scope_end = scope.find('\x1f');
    if (scope_end == std::string::npos || scope_end == 0)
      return false;
    const std::string key_prefix = scope.substr(0, scope_end + 1);
    if (source_scope_prefix.empty())
      source_scope_prefix = key_prefix;
    else if (source_scope_prefix != key_prefix)
      return false;  // a per-profile file must not mix source identities
    if (key_prefix != target_scope_prefix) {
      scope = target_scope_prefix + scope.substr(scope_end + 1);
      rekeyed = true;
    }
    auto root = std::make_shared<FsNode>();
    ReadNodeInto(&r, root.get());
    if (!r.ok || !loaded.emplace(scope, std::move(root)).second)
      return false;
  }
  if (!r.ok)
    return false;

  for (const auto& [scope, root] : loaded)
    MergeNodeInto(*root, OpfsRoot(scope).get());
  if (out_rekeyed)
    *out_rekeyed = rekeyed;
  return true;
}

}  // namespace

bool MbSaveOPFSScoped(const std::string& path,
                      const std::string& scope_prefix) {
  std::string o;
  o.append(kOpfsMagic, 8);
  const auto& roots = OpfsRoots();
  uint32_t n = 0;
  for (const auto& [scope, root] : roots)
    if (scope.rfind(scope_prefix, 0) == 0)
      ++n;
  WLen(&o, n);
  for (const auto& [scope, root] : roots) {
    if (scope.rfind(scope_prefix, 0) != 0)
      continue;
    WBlob(&o, scope);
    WNode(&o, root.get());
  }
  return base::WriteFile(base::FilePath::FromUTF8Unsafe(path), o);
}

bool MbLoadOPFSScoped(const std::string& path,
                      const std::string& scope_prefix,
                      bool* rekeyed) {
  if (rekeyed)
    *rekeyed = false;
  if (scope_prefix.empty() || scope_prefix.back() != '\x1f')
    return false;
  std::string buf;
  if (!base::ReadFileToString(base::FilePath::FromUTF8Unsafe(path), &buf))
    return false;
  return ParseAndMergeScopedOpfs(buf, scope_prefix, rekeyed);
}

void MbClearOPFSScoped(const std::string& scope_prefix) {
  auto& roots = OpfsRoots();
  for (auto it = roots.begin(); it != roots.end();) {
    if (it->first.rfind(scope_prefix, 0) == 0)
      it = roots.erase(it);
    else
      ++it;
  }
}

bool MbSaveOPFS(const std::string& path) {
  std::string o;
  o.append(kOpfsMagic, 8);
  const auto& roots = OpfsRoots();
  WLen(&o, static_cast<uint32_t>(roots.size()));
  for (const auto& [scope, root] : roots) {
    WBlob(&o, scope);
    WNode(&o, root.get());
  }
  return base::WriteFile(base::FilePath::FromUTF8Unsafe(path), o);
}

bool MbLoadOPFS(const std::string& path) {
  std::string buf;
  if (!base::ReadFileToString(base::FilePath::FromUTF8Unsafe(path), &buf))
    return false;
  if (buf.size() < 8 || buf.compare(0, 8, kOpfsMagic, 8) != 0)
    return false;
  OpfsReader r{buf, 8};
  uint32_t scope_count = r.Len();
  for (uint32_t i = 0; i < scope_count && r.ok; ++i) {
    std::string scope = r.Blob();
    if (!r.ok)
      return false;
    // merge onto the live (or fresh) scope root
    ReadNodeInto(&r, OpfsRoot(scope).get());
  }
  return r.ok;
}

}  // namespace mb
