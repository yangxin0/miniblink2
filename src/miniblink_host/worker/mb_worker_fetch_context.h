// mb_worker_fetch_context — a minimal blink::WebWorkerFetchContext so a Worker's
// subresource loads (importScripts(), fetch()/XHR inside the worker) route through the
// host's libcurl-backed loader, instead of dying at fetch-context creation.
//
// This is STEP 1 of the dedicated-worker bring-up (see PROGRESS.md "Workers"): it makes
// MbFrameClient::CreateWorkerFetchContext return a real context. On its own it does NOT
// start a worker — DedicatedWorker still never spins its thread until the worker-host
// factory client (mb_platform.cc) is wired to call OnWorkerHostCreated/OnScriptLoadStarted.
// So `new Worker(...)` still degrades gracefully (the existing render smoke guard holds);
// this only removes the missing-fetch-context blocker that step.
//
// Known follow-up: the loader factory hands back mb::MbURLLoader, whose body-loader task
// runner is the CURRENT (creation-thread) runner. GetURLLoaderFactory() is consulted on
// the WORKER thread, so once workers actually run, the loader must use the worker-thread
// runner. Harmless until then (the factory is created but never invoked).

#ifndef MINIBLINK_HOST_WORKER_MB_WORKER_FETCH_CONTEXT_H_
#define MINIBLINK_HOST_WORKER_MB_WORKER_FETCH_CONTEXT_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "third_party/blink/public/platform/web_worker_fetch_context.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader_factory.h"

namespace mb {

// A blink::URLLoaderFactory that ignores the network-service plumbing and hands back the
// host's own libcurl-backed loader for every request — the worker analogue of how the
// frame's subresources are loaded (MbFrameClient::CreateURLLoaderForTesting).
class MbWorkerURLLoaderFactory : public blink::URLLoaderFactory {
 public:
  MbWorkerURLLoaderFactory(std::string user_agent, std::string extra_headers);
  ~MbWorkerURLLoaderFactory() override;

  std::unique_ptr<blink::URLLoader> CreateURLLoader(
      const network::ResourceRequest& request,
      scoped_refptr<base::SingleThreadTaskRunner> freezable_task_runner,
      scoped_refptr<base::SingleThreadTaskRunner> unfreezable_task_runner,
      mojo::PendingRemote<blink::mojom::blink::KeepAliveHandle> keep_alive_handle,
      blink::BackForwardCacheLoaderHelper* back_forward_cache_loader_helper,
      blink::Vector<std::unique_ptr<blink::URLLoaderThrottle>> throttles)
      override;

 private:
  std::string user_agent_;
  std::string extra_headers_;
};

// The per-worker fetch context. All ten WebWorkerFetchContext pure-virtuals; the only
// substantive one is GetURLLoaderFactory(), which returns the libcurl-backed factory.
class MbWorkerFetchContext : public blink::WebWorkerFetchContext {
 public:
  // `top_frame_origin` is the serialized origin of the document that created the worker
  // (e.g. "https://example.com" or "null" for an opaque/about:blank page). A DEDICATED
  // worker MUST report a non-null top-frame origin (WorkerFetchContext DCHECKs that only
  // shared/service workers may have none), so this is required, not optional.
  MbWorkerFetchContext(std::string user_agent, std::string extra_headers,
                       std::string top_frame_origin);

  // blink::WebWorkerFetchContext:
  void SetTerminateSyncLoadEvent(base::WaitableEvent*) override;
  void InitializeOnWorkerThread(blink::AcceptLanguagesWatcher*) override;
  blink::URLLoaderFactory* GetURLLoaderFactory() override;
  std::unique_ptr<blink::URLLoaderFactory> WrapURLLoaderFactory(
      blink::CrossVariantMojoRemote<
          network::mojom::URLLoaderFactoryInterfaceBase> url_loader_factory)
      override;
  void FinalizeRequest(blink::WebURLRequest&) override;
  std::vector<std::unique_ptr<blink::URLLoaderThrottle>> CreateThrottles(
      const network::ResourceRequest& request) override;
  blink::mojom::ControllerServiceWorkerMode GetControllerServiceWorkerMode()
      const override;
  net::SiteForCookies SiteForCookies() const override;
  std::optional<blink::WebSecurityOrigin> TopFrameOrigin() const override;
  blink::WebString GetAcceptLanguages() const override;

  // Make a fresh context with the same identity, for a nested worker (the worker-host
  // factory client's CloneWorkerFetchContext).
  scoped_refptr<MbWorkerFetchContext> CloneContext() const;

 private:
  ~MbWorkerFetchContext() override;

  std::string user_agent_;
  std::string extra_headers_;
  std::string top_frame_origin_;  // serialized; rebuilt per call (worker-thread safe)
  std::unique_ptr<MbWorkerURLLoaderFactory> factory_;
};

}  // namespace mb

#endif  // MINIBLINK_HOST_WORKER_MB_WORKER_FETCH_CONTEXT_H_
