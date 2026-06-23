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

1. **Service thread scaffold.** Add `MbServiceThread`, start/stop in `MbRuntime`.
   Verify: suite still 76/76 (no behavior change yet); thread starts/joins cleanly.
2. **Validate cross-thread `[Sync]` servicing** (de-risk the core assumption *before*
   building BlobRegistry). Bind a tiny existing async interface on the service thread
   and confirm a round-trip; if feasible, exercise a `[Sync]` method to prove the main
   thread is serviced cross-thread without deadlock. (If this fails, stop — the whole
   approach is wrong.)
3. **`MbBlobRegistry.Register` (inline bytes only).** Route `BlobRegistry` to the
   service thread; store `embedded_data` by uuid; bind an `MbBlob`. Verify: `new Blob`
   still constructs, no hang, suite green. (Reads not wired yet → still pending, but no
   regression.)
4. **`MbBlob.ReadAll` via data pipe.** Serve stored bytes. Verify (NEW smoke):
   `new Blob(['hello']).text()` resolves to `"hello"`; `FileReader.readAsText`
   delivers; `arrayBuffer()` byteLength == 5. THIS is the user-visible win.
5. **Blob-URL resolution.** Revisit `patches/0003`: with a real store + service thread,
   `BlobURLStore`/`createObjectURL` can resolve (img `src=blobURL` loads). Verify:
   a blob: URL used as an `<img>`/fetch source loads instead of failing.
6. **BytesProvider path** for blobs > 256 KB (call back to the renderer). Verify with
   a large blob.

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
