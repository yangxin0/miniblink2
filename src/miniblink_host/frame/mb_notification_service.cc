#include "miniblink_host/frame/mb_notification_service.h"

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "base/functional/callback_helpers.h"
#include "base/no_destructor.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/mojom/notifications/notification.mojom-blink.h"
#include "third_party/blink/public/mojom/permissions/permission_status.mojom-blink.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace mb {
namespace {

using blink::mojom::blink::NonPersistentNotificationListener;
using blink::mojom::blink::NotificationService;

// Grants the notification permission and "shows" non-persistent notifications by firing the
// listener's OnShow() — no OS toast, but the Notification API is live (onshow/onclose run).
class MbNotificationService : public NotificationService {
 public:
  // [Sync] — backs the Notification.permission getter. Granted so notifications work.
  void GetPermissionStatus(GetPermissionStatusCallback callback) override {
    std::move(callback).Run(blink::mojom::blink::PermissionStatus::GRANTED);
  }

  void DisplayNonPersistentNotification(
      const blink::String& token,
      blink::mojom::blink::NotificationDataPtr data,
      blink::mojom::blink::NotificationResourcesPtr /*resources*/,
      mojo::PendingRemote<NonPersistentNotificationListener> event_listener)
      override {
    // Surface the notification's fields to the embedder (a native toast / its own UI) —
    // otherwise `new Notification(...)` is invisible to the host.
    if (data) {
      MbInvokeNotificationHook(data->title.Utf8(), data->body.Utf8(),
                               data->tag.Utf8(), data->icon.GetString().Utf8());
    }
    mojo::Remote<NonPersistentNotificationListener> listener(
        std::move(event_listener));
    listener->OnShow();  // fire Notification.onshow
    // Keep the listener alive (keyed by token) so a later close can notify it.
    listeners_[token.Utf8()] = std::move(listener);
  }

  void CloseNonPersistentNotification(const blink::String& token) override {
    auto it = listeners_.find(token.Utf8());
    if (it != listeners_.end()) {
      it->second->OnClose(base::DoNothing());  // fire Notification.onclose
      listeners_.erase(it);
    }
  }

  // Persistent (service-worker) notifications: accept but don't surface (no SW backend).
  void DisplayPersistentNotification(
      int64_t /*service_worker_registration_id*/,
      blink::mojom::blink::NotificationDataPtr /*data*/,
      blink::mojom::blink::NotificationResourcesPtr /*resources*/,
      DisplayPersistentNotificationCallback callback) override {
    std::move(callback).Run(
        blink::mojom::blink::PersistentNotificationError::NONE);
  }
  void ClosePersistentNotification(const blink::String& /*id*/) override {}
  void GetNotifications(int64_t /*service_worker_registration_id*/,
                        const blink::String& /*filter_tag*/,
                        bool /*include_triggered*/,
                        GetNotificationsCallback callback) override {
    std::move(callback).Run({}, {});  // none displayed
  }

 private:
  std::map<std::string, mojo::Remote<NonPersistentNotificationListener>>
      listeners_;
};

MbNotificationHook& NotificationHook() {
  static base::NoDestructor<MbNotificationHook> hook;
  return *hook;
}

}  // namespace

void MbSetNotificationHook(MbNotificationHook hook) {
  NotificationHook() = std::move(hook);
}

void MbInvokeNotificationHook(const std::string& title, const std::string& body,
                              const std::string& tag, const std::string& icon) {
  if (NotificationHook())
    NotificationHook()(title, body, tag, icon);
}

void BindNotificationService(
    mojo::PendingReceiver<blink::mojom::blink::NotificationService> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbNotificationService>(),
                              std::move(receiver));
}

}  // namespace mb
