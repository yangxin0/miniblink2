// mb_notification_service — an in-process Notifications (Notification API) backend.
//
// blink requests a mojom::NotificationService from the frame's BrowserInterfaceBroker.
// Without one, Notification.permission hangs (it's a [Sync] call) and `new Notification`
// never fires onshow. This grants the permission and "displays" non-persistent
// notifications by firing the listener's OnShow() — so a page's Notification.onshow runs
// (headless: no OS toast, but the API is live and scriptable).

#ifndef MINIBLINK_HOST_FRAME_MB_NOTIFICATION_SERVICE_H_
#define MINIBLINK_HOST_FRAME_MB_NOTIFICATION_SERVICE_H_

#include <functional>
#include <string>

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/notifications/notification_service.mojom-blink.h"

namespace mb {

// Bind a NotificationService receiver to the in-process backend (self-owned). Bound on the
// broker's service thread so the [Sync] GetPermissionStatus is answered while the main
// thread blocks.
void BindNotificationService(
    mojo::PendingReceiver<blink::mojom::blink::NotificationService> receiver);

// A process-wide hook fired when a page DISPLAYS a non-persistent Notification
// (new Notification(title, {body, tag, icon})). The embedder receives the notification's
// fields and can surface it (a native toast / its own UI). Set {} to clear. Process-wide
// because the NotificationService is not frame-scoped (like the loader request hooks).
using MbNotificationHook =
    std::function<void(const std::string& title, const std::string& body,
                       const std::string& tag, const std::string& icon)>;
void MbSetNotificationHook(MbNotificationHook hook);
void MbInvokeNotificationHook(const std::string& title, const std::string& body,
                              const std::string& tag, const std::string& icon);

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_NOTIFICATION_SERVICE_H_
