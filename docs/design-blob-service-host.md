# Design — Off-main-thread mojo service host (Blob data, then workers)

**Status:** plan (2026-06-24). **Scope:** the one remaining heavy enabler behind the
last functional gaps. Not yet implemented — this doc scopes a multi-step effort so it
can be executed incrementally, each step independently buildable and verified by
`mb_smoke` (no broken intermediate states).

## Why

Across the robustness sweeps, every blocking failure reduced to **a `[Sync]` mojo call
the main thread makes to a service that, in production, lives in the browser process**.
We closed the ones that can be auto-dismissed (JS dialogs) or skipped (blob-URL
register). The remaining ones need the call to actually be *serviced* and *reply*:

- `BlobRegistry.Register` (`blob_registry.mojom`, **`[Sync]`**) — so `blob.text()` /
  `arrayBuffer()` / `FileReader` / blob-URL loads resolve to real bytes. Today
  `new Blob()` doesn't hang only because `MbEmptyBroker` drops the receiver, so the
  sync Register fails fast and the blob is never registered (reads stay pending).
- Worker bring-up wants the same: a real `DedicatedWorkerHost` + a worker thread with
  its own platform/scheduler/loader, reachable over mojo.

**The deadlock:** a `[Sync]` call blocks the calling (main) thread until it gets a
reply. An in-process receiver bound on the *same* thread can never run to produce that
reply (its task is queued behind the blocked sync wait) — confirmed empirically with
the dialogs. Mojo *does* support servicing a sync call from a receiver on **another
thread/sequence** in the same process (that is exactly how the browser process answers
it). So the fix is a dedicated **service thread** that hosts these receivers.

## Findings from investigation (2026-06-24) — these simplify the plan

- **The service thread already exists.** `MbRuntime` starts `io_thread_` (`mb-io`, a
  `base::Thread` with `MessagePumpType::IO`) and attaches `mojo::core::ScopedIPCSupport`
  to it (mb_runtime.cc:146-151). Bind the blob receivers there — do NOT spawn a new
  thread. Original "increment 1 (scaffold)" is therefore essentially already done.
- **`[Sync]` servicing already works in-process.** `MimeRegistry.GetMimeTypeFromExtension`
  is `[Sync]`, bound by `MbEmptyBroker` on the **main thread**, and a `file://`
  stylesheet loads + applies correctly through it (verified: external CSS sets
  `color:rgb(1,2,3)`, no hang). It doesn't deadlock because that `[Sync]` call's caller
  is a resource-loading sequence, not the blocked main thread — i.e. the cross-sequence
  `[Sync]` mechanism is functional in our mojo setup.
- **We provide the servicer; Blink makes the sync call.** For the real integration we do
  NOT make any sync call ourselves — Blink's `PublicURLManager` / blob plumbing calls
  `BlobRegistry.Register` `[Sync]` exactly as in production. So the friend-gated
  `mojo::SyncCallRestrictions::ScopedAllowSyncCall` (unusable outside its allowlist) is a
  non-issue for the integration; it would only bite a *standalone* validation experiment.
  => The standalone "increment 2" experiment can be SKIPPED; go straight to binding a real
  `BlobRegistry` on `io_thread_` and measuring `blob.text()`.
- **Narrowed remaining unknown:** the only unproven pairing is main-thread-CALLER +
  io_thread-SERVICER (blob's case; MimeRegistry's caller is off-main). Standard mojo
  services a sync call on the receiver's thread regardless of caller, so this is expected
  to work — increment 3's `blob.text()` smoke is the authoritative check.

## Architecture

```
 main thread (Blink)                         service thread (MbServiceThread)
 ──────────────────                          ────────────────────────────────
 new Blob(['x'])                              MbBlobRegistry (bound here)
   -> BlobRegistry.Register [Sync] ──────────►  store bytes by uuid, bind MbBlob
   <──────────────────────────── reply ()        (reply unblocks main thread)
 blob.text()                                  MbBlob.ReadAll / AsDataPipeGetter
   -> async read ───────────────────────────►  write stored bytes into data pipe
   <──────────── data pipe + done ────────────
```

- **`runtime/mb_service_thread.{h,cc}`** — a `base::Thread` (IO-capable: started with
  `base::MessagePumpType::IO` so mojo can watch pipe handles) exposing its
  `scoped_refptr<base::SequencedTaskRunner>`. Owned by `MbRuntime`, started in
  `Initialize()`, joined in `Shutdown()`.
- Brokers route the relevant interfaces to the service thread instead of dropping them:
  - `MbEmptyBroker::GetInterfaceImpl` (platform broker) and/or
    `MbBrowserInterfaceBroker` (frame broker) — for a `BlobRegistry` receiver, post a
    `MakeSelfOwnedReceiver` onto the service-thread task runner.
- **`blob/mb_blob_registry.{h,cc}`** — implements `blink::mojom::BlobRegistry` +
  `blink::mojom::Blob`, lives entirely on the service thread.

## Mojo facts that shape the plan

- `BlobRegistry.Register(pending_receiver<Blob>, uuid, type, disposition,
  array<DataElement>) => ()` — `[Sync]`, empty reply (just an ack).
- `DataElement` is a union; the renderer path uses `DataElementBytes { length,
  array<uint8>? embedded_data, pending_remote<BytesProvider> data }`.
  - **embedded_data is inline for blobs ≤ 256 KB** (`kMaximumEmbeddedDataSize`) — the
    common case (`new Blob(['hello'])`). The first increment handles ONLY this: copy
    `embedded_data` into the store, ignore the `BytesProvider`.
  - Larger blobs need calling back `BytesProvider.RequestAsReply() => (bytes)` /
    `RequestAsStream(pipe)` from the service thread — a later increment.
- `Blob` reads: `ReadAll(data_pipe_producer, client)`, `ReadRange(...)`,
  `AsDataPipeGetter`, `ReadSideData`, `CaptureSnapshot`, `Clone`, `GetInternalUUID`.
  First increment implements `ReadAll`/`ReadRange`/`CaptureSnapshot`/`Clone`/
  `GetInternalUUID`; the rest can `NOTIMPLEMENTED()` initially.

## Increments (each ends green: build + `mb_smoke`, no survivors)

(Revised 2026-06-24 per findings above: the service thread is `io_thread_`, and the
standalone sync-validation experiment is dropped.)

1. **~~Service thread scaffold~~ — DONE.** Reuse the existing `io_thread_`. No new code;
   the plan binds blob receivers there.
2. **~~Standalone cross-thread `[Sync]` validation~~ — DROPPED.** We don't make the sync
   call (Blink does), and `[Sync]` servicing is already shown working in-process. The
   `blob.text()` smoke in increment 4 is the real check.
3. **`MbBlobRegistry.Register` (inline bytes only), bound on `io_thread_`.** Route
   `BlobRegistry` (and the blob-bytes path) to the service thread via the platform
   broker; on `Register`, copy each `DataElementBytes.embedded_data` into a store keyed
   by uuid and bind an `MbBlob` (also on `io_thread_`). Verify: `new Blob` still
   constructs, no hang, suite green. (Reads not wired yet → still pending, no regression.)
4. **`MbBlob.ReadAll`/`ReadRange` via data pipe.** Serve stored bytes (chunk on
   `MOJO_RESULT_SHOULD_WAIT` with a `SimpleWatcher` on `io_thread_`). Verify (NEW smoke):
   `new Blob(['hello']).text()` resolves to `"hello"`; `FileReader.readAsText` delivers;
   `arrayBuffer().byteLength == 5`. THIS is the user-visible win.
5. **Blob-URL resolution.** Revisit `patches/0003`: with a real store + service thread,
   `BlobURLStore`/`createObjectURL` can resolve (img `src=blobURL` loads). Verify:
   a blob: URL used as an `<img>`/fetch source loads instead of failing.
6. **BytesProvider path** for blobs > 256 KB (call back to the renderer via
   `BytesProvider.RequestAsReply`/`RequestAsStream`). Verify with a large blob.

## Implementation notes — ready to execute (confirmed 2026-06-24)

All unknowns resolved by code inspection; increment 3+4 is now mechanical:

- **Broker:** `BlobRegistry` is requested through the **Platform** broker —
  `blob_data.cc:93` does `Platform::Current()->GetBrowserInterfaceBroker()
  ->GetInterface(BlobRegistry receiver)` (a thread-specific remote). So route it in
  `MbEmptyBroker::GetInterfaceImpl` (mb_platform.cc), matching
  `mojom::blink::BlobRegistry`. (NOT the frame broker; that's BlobURLStore, increment 5.)
- **Get the service runner to the broker:** add `MbRuntime::ServiceTaskRunner()`
  returning `io_thread_->task_runner()`; `MbEmptyBroker` posts the bind there:
  `runner->PostTask(FROM_HERE, BindOnce([](PendingReceiver<BlobRegistry> r){
  MakeSelfOwnedReceiver(make_unique<MbBlobRegistry>(), std::move(r)); }, std::move(r)))`.
  Everything blob (registry + every `MbBlob`) lives on `io_thread_`.
- **Variant:** blink variant (`...mojom-blink.h`) — `WTF::String` uuid, `WTF::Vector<
  DataElementPtr>` elements, `WTF::Vector<uint8_t>` embedded_data.
- **`MbBlobRegistry::Register(receiver, uuid, type, disposition, elements)`:** for each
  `element` with `element->is_bytes()`, append `element->get_bytes()->embedded_data`
  (present for ≤256 KB) to a `Vector<uint8_t>`; `MakeSelfOwnedReceiver(make_unique<MbBlob>
  (uuid, bytes), std::move(receiver))`; the `=> ()` reply is sent automatically when the
  handler returns (this is what unblocks Blink's `[Sync]` Register). `RegisterFromStream`:
  minimal / `NOTIMPLEMENTED` initially.
- **`MbBlob` (holds uuid + bytes):**
  - `ReadAll(producer_handle, client)` — **the read path** (`file_reader_loader.cc:102`).
    Bind `mojo::Remote<BlobReaderClient>` from `client` (on io_thread); call
    `client->OnCalculatedSize(size, size)`; write bytes to `producer_handle`
    (small blobs fit one `WriteData`; chunk via `SimpleWatcher` later); call
    `client->OnComplete(/*net::OK=*/0, size)`; drop the producer (EOF). `client` is
    optional (may be null) — guard it.
  - `GetInternalUUID(cb)` → `cb.Run(uuid)`. `Clone(receiver)` → new `MbBlob` same bytes.
  - `CaptureSnapshot(cb)` `[Sync]` → `cb.Run(size, std::nullopt)`.
  - `ReadRange`, `AsDataPipeGetter`, `ReadSideData`, `Load` → `NOTIMPLEMENTED`/no-op first.
- **Smoke (the win):** `new Blob(['hello']).text()` → `"hello"`;
  `new Blob([...]).arrayBuffer().byteLength`; `FileReader.readAsText`. Bounded-pump,
  watchdog, no survivors. THIS proves the main-thread-caller + io_thread-servicer pairing.
- **GN:** new `blob/mb_blob_registry.{h,cc}` added to `miniblink_host` sources; deps already
  cover `//third_party/blink/public:blink` (mojom-blink) and mojo bindings.

## Increment 6 — large blobs (>256 KB), ready to execute (scoped 2026-06-24)

Status: increments 3+4 SHIPPED (small/inline blobs read via `MbBlob::ReadAll`).
Measured: blobs up to **200 KB round-trip fine** (inline `embedded_data` + one
`WriteAllData`); only **>256 KB** fails — `text()` resolves EMPTY because there is no
`embedded_data` and we ignore the `BytesProvider`. Two pieces, both needed, atomic:

1. **Fetch via `BytesProvider`.** In `Register`, build an ordered list of parts; for a
   bytes element with no `embedded_data`, bind its `data` (`PendingRemote<BytesProvider>`)
   on the service thread and remember it. Reply to `Register` IMMEDIATELY (must unblock
   the main thread — the provider lives on that thread and can't answer while it's
   blocked). After the reply, lazily materialize: walk parts in order, for each provider
   call `RequestAsReply() => (bytes)` (async; main thread is now unblocked so it
   services it), append to `data_`. Gate reads: if a `ReadAll` arrives before
   materialization completes, queue `{pipe, client}` and drain when ready. Capturing raw
   `this` in the reply callback is safe — the `Remote<BytesProvider>` is a member, so
   destroying `MbBlob` cancels the pending callback (no UAF).
2. **Chunked pipe write.** `WriteAllData` (ALL_OR_NONE) fails once `data_` exceeds the
   pipe capacity (~256 KB+). Replace with a `mojo::SimpleWatcher` on the service thread:
   write what fits, on `MOJO_RESULT_SHOULD_WAIT` arm the watcher for writable and
   continue, fire `OnComplete` + drop the producer at the end.

Verify (NEW smoke): `new Blob(['x'.repeat(500000)]).text().length === 500000`;
`arrayBuffer().byteLength === 500000`. Watchdog-bounded (hang risk if the watcher never
arms or a provider stalls — test must catch it). Risk: this is real async state — execute
in a focused pass with the watchdog, revert if it can't be made clean in-tick.

## Risks / open questions

- IO-pump thread + Blink's mojo core: confirm `mojo::core` is initialized in a mode
  that allows binding receivers on a second thread (it is for multi-threaded use; the
  runtime already calls `mojo::core::Init`). Increment 2 validates this.
- Data-pipe backpressure: `ReadAll` must write in chunks if the blob exceeds the pipe
  capacity; handle `MOJO_RESULT_SHOULD_WAIT` with a `SimpleWatcher` on the service
  thread.
- Lifetime: blobs are ref-counted across renderer handles (`Clone`); store must
  keep bytes alive until all `MbBlob` receivers for a uuid are gone.

## Why this also unblocks workers

A `DedicatedWorker` needs a `WebDedicatedWorkerHostFactoryClient` that actually creates
a host + worker thread (today it's the inert stub from the worker-crash fix). The same
service thread can host the worker's browser-side mojo endpoints; worker *execution*
additionally needs a `WorkerThread` with its own `Platform`/scheduler/loader, which is
a separate large effort, but it shares this service-host foundation. Blob is the
smaller, self-contained first customer — do it first.
