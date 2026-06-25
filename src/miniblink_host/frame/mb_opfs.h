// mb_opfs.h — in-process Origin Private File System (navigator.storage.getDirectory()).
//
// Backs blink.mojom.FileSystemAccessManager with an in-memory directory/file tree. Slice 1
// implements the tree (create/navigate/enumerate/remove directories and files); file CONTENT
// read/write (AsBlob/CreateFileWriter/OpenAccessHandle) is deferred and rejects cleanly.

#ifndef MINIBLINK_HOST_FRAME_MB_OPFS_H_
#define MINIBLINK_HOST_FRAME_MB_OPFS_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_manager.mojom-blink-forward.h"

namespace mb {

void BindFileSystemAccessManager(
    mojo::PendingReceiver<blink::mojom::blink::FileSystemAccessManager> receiver);

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_OPFS_H_
