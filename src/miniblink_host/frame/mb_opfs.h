// mb_opfs.h — in-process Origin Private File System (navigator.storage.getDirectory()).
//
// Backs blink.mojom.FileSystemAccessManager with an in-memory directory/file tree. Slice 1
// implements the tree (create/navigate/enumerate/remove directories and files); file CONTENT
// read/write (AsBlob/CreateFileWriter/OpenAccessHandle) is deferred and rejects cleanly.

#ifndef MINIBLINK_HOST_FRAME_MB_OPFS_H_
#define MINIBLINK_HOST_FRAME_MB_OPFS_H_

#include <cstdint>
#include <string>

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_directory_handle.mojom-blink-forward.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_manager.mojom-blink-forward.h"

namespace mb {

// `frame_key` -> the frame's origin, which scopes the OPFS root so navigator.storage
// .getDirectory() is ISOLATED per origin (a process-wide root would leak files
// across origins, the same security gap fixed for IndexedDB).
void BindFileSystemAccessManager(
    mojo::PendingReceiver<blink::mojom::blink::FileSystemAccessManager> receiver,
    uint64_t frame_key);

// Bind a directory handle for the OPFS root of storage `scope` (an origin, or
// origin+bucket) — used by Storage Buckets' getDirectory().
mojo::PendingRemote<blink::mojom::blink::FileSystemAccessDirectoryHandle>
MbBindOpfsRootDirectory(const std::string& scope);

// Persist the WHOLE in-memory OPFS tree (every origin/bucket scope, directories + file
// bytes) to a private binary file, and restore it. Load MERGES onto the live tree (updates
// existing nodes in place / creates missing ones, never deletes), so it is safe to call at
// any time — existing directory/file handles stay valid. Returns false on I/O / format
// error. (navigator.storage.getDirectory() files survive a save/load across sessions.)
bool MbSaveOPFS(const std::string& path);
bool MbLoadOPFS(const std::string& path);

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_OPFS_H_
