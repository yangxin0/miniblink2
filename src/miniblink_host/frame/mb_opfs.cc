#include "miniblink_host/frame/mb_opfs.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_cloud_identifier.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_directory_handle.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_error.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_file_handle.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_manager.mojom-blink.h"
#include "third_party/blink/public/mojom/permissions/permission_status.mojom-blink.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace mb {
namespace {

namespace m = blink::mojom::blink;

// An in-memory OPFS node: a directory (named children) or a file (raw bytes). The root is
// process-wide (a headless host is effectively single-origin) and persists across
// getDirectory() calls within a run, so files created earlier are seen again.
struct FsNode {
  bool is_dir = true;
  std::map<std::string, std::unique_ptr<FsNode>> children;  // when is_dir
  std::string bytes;                                        // when !is_dir
};

FsNode* OpfsRoot() {
  static FsNode* root = new FsNode();  // is_dir = true
  return root;
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
  // File content read/write is deferred (slice 2): reject fast, never hang.
  void AsBlob(AsBlobCallback cb) override {
    std::move(cb).Run(NotSupported("OPFS file read not yet supported"),
                      base::File::Info(), nullptr);
  }
  void CreateFileWriter(bool, bool,
                        m::FileSystemAccessWritableFileStreamLockMode,
                        CreateFileWriterCallback cb) override {
    std::move(cb).Run(NotSupported("OPFS file write not yet supported"),
                      mojo::NullRemote());
  }
  void OpenAccessHandle(m::FileSystemAccessAccessHandleLockMode,
                        OpenAccessHandleCallback cb) override {
    std::move(cb).Run(NotSupported("OPFS access handle not supported"), nullptr,
                      mojo::NullRemote());
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
  // Holds the file's bytes; read/write lands here in slice 2 (unused for the tree slice).
  [[maybe_unused]] FsNode* node_;
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
  void GetSandboxedFileSystem(GetSandboxedFileSystemCallback cb) override {
    std::move(cb).Run(Ok(), BindDir(OpfsRoot()));
  }
  void GetSandboxedFileSystemForDevtools(
      const blink::Vector<blink::String>&,
      GetSandboxedFileSystemForDevtoolsCallback cb) override {
    std::move(cb).Run(Ok(), BindDir(OpfsRoot()));
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
};

}  // namespace

void BindFileSystemAccessManager(
    mojo::PendingReceiver<blink::mojom::blink::FileSystemAccessManager>
        receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbFileSystemAccessManager>(),
                              std::move(receiver));
}

}  // namespace mb
