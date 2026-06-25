// mb_notification_service — an in-process Notifications (Notification API) backend.
//
// blink requests a mojom::NotificationService from the frame's BrowserInterfaceBroker.
// Without one, Notification.permission hangs (it's a [Sync] call) and `new Notification`
// never fires onshow. This grants the permission and "displays" non-persistent
// notifications by firing the listener's OnShow() — so a page's Notification.onshow runs
// (headless: no OS toast, but the API is live and scriptable).

#ifndef MINIBLINK_HOST_FRAME_MB_NOTIFICATION_SERVICE_H_
#define MINIBLINK_HOST_FRAME_MB_NOTIFICATION_SERVICE_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/notifications/notification_service.mojom-blink.h"

namespace mb {

// Bind a NotificationService receiver to the in-process backend (self-owned). Bound on the
// broker's service thread so the [Sync] GetPermissionStatus is answered while the main
// thread blocks.
void BindNotificationService(
    mojo::PendingReceiver<blink::mojom::blink::NotificationService> receiver);

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_NOTIFICATION_SERVICE_H_
