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
struct FsNode {
  bool is_dir = true;
  std::map<std::string, std::unique_ptr<FsNode>> children;  // when is_dir
  std::string bytes;                                        // when !is_dir
};

FsNode* OpfsRoot(const std::string& scope) {
  static auto* roots =
      new std::map<std::string, std::unique_ptr<FsNode>>();  // scope -> root
  auto it = roots->find(scope);
  if (it == roots->end())
    it = roots->emplace(scope, std::make_unique<FsNode>()).first;
  return it->second.get();
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

mojo::PendingRemote<m::FileSystemAccessFileHandle> BindFile(FsNode* node);
mojo::PendingRemote<m::FileSystemAccessDirectoryHandle> BindDir(FsNode* node);

// ----------------------------- file writer -----------------------------
// A writable session over one file. Writes accumulate into a working buffer (a copy of the
// file's bytes when keep_existing_data); Close() commits the buffer back to the node, Abort()
// discards it. Each Write drains its data pipe, then splices the bytes in at the given offset.
class MbFsFileWriter : public m::FileSystemAccessFileWriter {
 public:
  MbFsFileWriter(FsNode* node, bool keep_existing_data)
      : node_(node),
        buffer_(keep_existing_data ? node->bytes : std::string()) {}

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

  FsNode* node_;
  std::string buffer_;
  std::vector<std::unique_ptr<WriteOp>> ops_;
};

// ------------------------- sync access handle --------------------------
// The in-memory ("incognito") delegate behind createSyncAccessHandle(). A worker reads/writes
// the file synchronously via these [Sync] RPCs; they operate directly on the node's bytes.
class MbFsFileDelegateHost : public m::FileSystemAccessFileDelegateHost {
 public:
  explicit MbFsFileDelegateHost(FsNode* node) : node_(node) {}

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
  FsNode* node_;
};

// The browser-side handle whose Close() releases the file lock — a no-op ack here.
class MbFsAccessHandleHost : public m::FileSystemAccessAccessHandleHost {
 public:
  void Close(CloseCallback cb) override { std::move(cb).Run(); }
};

// ----------------------------- file handle -----------------------------
class MbFsFileHandle : public m::FileSystemAccessFileHandle {
 public:
  explicit MbFsFileHandle(FsNode* node) : node_(node) {}

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
                        m::FileSystemAccessWritableFileStreamLockMode,
                        CreateFileWriterCallback cb) override {
    mojo::PendingRemote<m::FileSystemAccessFileWriter> remote;
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbFsFileWriter>(node_, keep_existing_data),
        remote.InitWithNewPipeAndPassReceiver());
    std::move(cb).Run(Ok(), std::move(remote));
  }
  // createSyncAccessHandle() (Worker-only): hand back an in-memory file delegate the worker
  // drives synchronously, plus a host whose Close() releases the (notional) lock.
  void OpenAccessHandle(m::FileSystemAccessAccessHandleLockMode,
                        OpenAccessHandleCallback cb) override {
    mojo::PendingRemote<m::FileSystemAccessFileDelegateHost> delegate;
    mojo::MakeSelfOwnedReceiver(std::make_unique<MbFsFileDelegateHost>(node_),
                                delegate.InitWithNewPipeAndPassReceiver());
    mojo::PendingRemote<m::FileSystemAccessAccessHandleHost> host;
    mojo::MakeSelfOwnedReceiver(std::make_unique<MbFsAccessHandleHost>(),
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
  FsNode* node_;  // the file node whose bytes back read (AsBlob) and write sessions
};

// --------------------------- directory handle ---------------------------
class MbFsDirectoryHandle : public m::FileSystemAccessDirectoryHandle {
 public:
  explicit MbFsDirectoryHandle(FsNode* node) : node_(node) {}

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
      auto child = std::make_unique<FsNode>();
      child->is_dir = false;
      it = node_->children.emplace(name, std::move(child)).first;
    } else if (it->second->is_dir) {
      std::move(cb).Run(
          FileError(base::File::FILE_ERROR_NOT_A_FILE, "name is a directory"),
          mojo::NullRemote());
      return;
    }
    std::move(cb).Run(Ok(), BindFile(it->second.get()));
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
      it = node_->children.emplace(name, std::make_unique<FsNode>())
               .first;  // is_dir = true
    } else if (!it->second->is_dir) {
      std::move(cb).Run(
          FileError(base::File::FILE_ERROR_NOT_A_DIRECTORY, "name is a file"),
          mojo::NullRemote());
      return;
    }
    std::move(cb).Run(Ok(), BindDir(it->second.get()));
  }
  void GetEntries(mojo::PendingRemote<m::FileSystemAccessDirectoryEntriesListener>
                      listener) override {
    mojo::Remote<m::FileSystemAccessDirectoryEntriesListener> l(
        std::move(listener));
    blink::Vector<m::FileSystemAccessEntryPtr> entries;
    for (auto& kv : node_->children) {
      m::FileSystemAccessHandlePtr handle =
          kv.second->is_dir
              ? m::FileSystemAccessHandle::NewDirectory(BindDir(kv.second.get()))
              : m::FileSystemAccessHandle::NewFile(BindFile(kv.second.get()));
      entries.push_back(m::FileSystemAccessEntry::New(
          std::move(handle), blink::String::FromUtf8(kv.first)));
    }
    l->DidReadDirectory(Ok(), std::move(entries), /*has_more_entries=*/false);
    listeners_.push_back(std::move(l));  // keep alive until the message flushes
  }
  void RemoveEntry(const blink::String& basename, bool /*recurse*/,
                   RemoveEntryCallback cb) override {
    std::string name = basename.Utf8();
    auto it = node_->children.find(name);
    if (it == node_->children.end()) {
      std::move(cb).Run(
          FileError(base::File::FILE_ERROR_NOT_FOUND, "entry not found"));
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
  FsNode* node_;
  std::vector<mojo::Remote<m::FileSystemAccessDirectoryEntriesListener>>
      listeners_;
};

mojo::PendingRemote<m::FileSystemAccessFileHandle> BindFile(FsNode* node) {
  mojo::PendingRemote<m::FileSystemAccessFileHandle> remote;
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbFsFileHandle>(node),
                              remote.InitWithNewPipeAndPassReceiver());
  return remote;
}
mojo::PendingRemote<m::FileSystemAccessDirectoryHandle> BindDir(FsNode* node) {
  mojo::PendingRemote<m::FileSystemAccessDirectoryHandle> remote;
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbFsDirectoryHandle>(node),
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

}  // namespace mb
