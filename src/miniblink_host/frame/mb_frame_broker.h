// mb_frame_broker — the in-process BrowserInterfaceBroker handed to the main frame.
//
// The frame resolves a few browser-process Mojo services through its
// BrowserInterfaceBroker (distinct from Platform's broker). We only need one:
// RestrictedCookieManager, so window.document.cookie reads/writes work. Everything
// else is dropped (no browser process).
//
// The returned remote's receiver is self-owned (kept alive by the live pipe), so the
// frame can hold the remote for its lifetime.
#ifndef MINIBLINK_HOST_FRAME_MB_FRAME_BROKER_H_
#define MINIBLINK_HOST_FRAME_MB_FRAME_BROKER_H_

#include <functional>
#include <string>

#include <cstdint>

#include "mojo/public/cpp/bindings/pending_remote.h"
#include "third_party/blink/public/mojom/browser_interface_broker.mojom-blink.h"

namespace mb {

// `frame_key` identifies the owning frame so per-origin storage (IndexedDB) can
// scope by the frame's current document origin (via MbGetFrameOrigin). Pass 0 for
// workers / when no frame is associated (origin unknown -> unscoped bucket).
mojo::PendingRemote<blink::mojom::blink::BrowserInterfaceBroker>
MakeFrameInterfaceBroker(uint64_t frame_key);

// Configure the geolocation fix served to navigator.geolocation (process-wide). Once
// set, getCurrentPosition/watchPosition resolve to (lat,lng) with `accuracy` metres
// (and the GeolocationService grants); MbClearGeolocation reverts to denied. Thread-safe.
void MbSetGeolocation(double lat, double lng, double accuracy);
void MbClearGeolocation();

// The in-process text clipboard shared by navigator.clipboard / execCommand and the
// host. MbSetClipboardText makes the page's next paste/readText see `text`;
// MbGetClipboardText reads what the page (or host) last wrote. Thread-safe.
void MbSetClipboardText(const std::string& text);
std::string MbGetClipboardText();

// Optional host OS-clipboard bridge (mbSetClipboardHandler / IMPROVEMENT.md
// item 34): with `read` installed, page clipboard READS (paste, readText) pull
// from it instead of the jar; with `write` installed, page WRITES (copy/cut,
// writeText) push to it as well as the jar (so MbGetClipboardText keeps
// working). Either may be null — that direction stays jar-only. The callbacks
// run on the broker's SERVICE thread; keep them thread-safe and cheap.
void MbSetClipboardHandler(std::function<std::string()> read,
                           std::function<void(const std::string&)> write);

// Clear the page-visible document.cookie / Cookie Store mirror for one profile.
// The HTTP jar is cleared separately by MbClearCookieJar. Safe to call from the
// engine thread; mutation is queued onto the broker's service thread.
void MbClearPageCookieStoreForSession(const std::string& session_key);

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_FRAME_BROKER_H_
