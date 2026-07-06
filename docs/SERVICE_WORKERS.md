# Service Workers — the staged plan

Status: **planned, not started** (stages below are the commitment; BACKLOG A1's
"declined" is superseded by an explicit request, 2026-07-06). The prime
directive from A1 stands: **no fake success** — `register()` keeps rejecting
cleanly until the stage that makes the promise true has shipped. A site must
never observe a registered-but-inert service worker.

## Why now

SW is the one web-platform gap that blocks whole classes of modern sites
(offline apps, PWA-shaped SPAs, push-less notification queues). It matters if
miniblink2 is a general embedding SDK rather than only Glyph's engine + the
automation embedder.

## Scope reality (from the A1 investigation)

28 service-worker mojom interfaces, ~40 renderer files, plus the browser-side
stack reimplemented in-process: container/registration/object hosts, embedded
worker startup, the install→activate state machine, ControllerServiceWorker +
fetch-event dispatch with response streams, installed-scripts caching. The
renderer half is compiled into the binary already; the work is the in-process
"browser" half, like every other subsystem here (workers, IDB, cache_storage).

## Stages (each lands green or not at all)

**S1 — registration + lifecycle skeleton.** In-process
ServiceWorkerContainerHost + Registration/ObjectHosts; `register()` resolves
with a real registration; script fetched through the worker main-script path
(per-view mock hook + session cookies apply, same as dedicated workers);
`installing→waiting→active` driven WITHOUT interception — but gated OFF by
default (`mbEnableServiceWorkers(1)`) until S3, honoring no-fake-success.
Exit test: register/unregister round trip, updatefound/statechange events.

**S2 — the worker actually runs.** Embedded-worker startup on a worker thread
(reuse the dedicated-worker infra), ServiceWorkerGlobalScope, install/activate
events with waitUntil, skipWaiting/clients.claim, client postMessage both ways.
Exit test: an install-event SW that caches.addAll()s and activates (cache
storage already works — the B1 lifetime fix was a prerequisite).

**S3 — fetch interception (the point of it all).** ControllerServiceWorker;
controlled clients' subresource + navigation fetches dispatch FetchEvent;
respondWith streams into MbURLLoader; no-respondWith falls through to the
network path unchanged. This is the hard stage: response streaming, the
fallback race, and the loader integration. Exit test: an offline page served
entirely from a SW after the network is mocked away; the SW-less path shows
zero regressions in both smoke suites.

**S4 — persistence + update.** Installed scripts stored per session (under
persist_dir like cookies/IDB/OPFS); byte-identical update check on navigation;
unregister wipes. Exit test: a persistent session restarts and the SW controls
the first navigation offline.

Deliberately out of scope: push, background sync, navigation preload (reject/
absent cleanly, like today).

## Order of magnitude

S1+S2 ≈ a week each, S3 the long pole (1–2 weeks), S4 days. Nothing else in
the backlog competes with this for size; it should not be started casually or
squeezed between other work.
