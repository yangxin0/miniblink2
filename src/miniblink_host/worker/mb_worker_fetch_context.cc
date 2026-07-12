#include "miniblink_host/worker/mb_worker_fetch_context.h"

#include <utility>

#include "miniblink_host/loader/mb_url_loader.h"

namespace mb {

MbWorkerURLLoaderFactory::MbWorkerURLLoaderFactory(
    std::string user_agent,
    std::string extra_headers,
    scoped_refptr<MbLoaderViewContext> view_context)
    : user_agent_(std::move(user_agent)),
      extra_headers_(std::move(extra_headers)),
      view_context_(std::move(view_context)) {}

MbWorkerURLLoaderFactory::~MbWorkerURLLoaderFactory() = default;

std::unique_ptr<blink::URLLoader> MbWorkerURLLoaderFactory::CreateURLLoader(
    const network::ResourceRequest& /*request*/,
    scoped_refptr<base::SingleThreadTaskRunner> /*freezable_task_runner*/,
    scoped_refptr<base::SingleThreadTaskRunner> /*unfreezable_task_runner*/,
    mojo::PendingRemote<blink::mojom::blink::KeepAliveHandle> /*keep_alive*/,
    blink::BackForwardCacheLoaderHelper* /*bf_cache_loader_helper*/,
    blink::Vector<std::unique_ptr<blink::URLLoaderThrottle>> /*throttles*/) {
  // Same libcurl-backed loader the frame uses for subresources.
  return std::make_unique<MbURLLoader>(user_agent_, extra_headers_,
                                       view_context_);
}

MbWorkerFetchContext::MbWorkerFetchContext(std::string user_agent,
                                           std::string extra_headers,
                                           std::string top_frame_origin,
                                           scoped_refptr<MbLoaderViewContext>
                                               view_context)
    : user_agent_(std::move(user_agent)),
      extra_headers_(std::move(extra_headers)),
      top_frame_origin_(std::move(top_frame_origin)),
      view_context_(std::move(view_context)),
      factory_(std::make_unique<MbWorkerURLLoaderFactory>(user_agent_,
                                                          extra_headers_,
                                                          view_context_)) {}

MbWorkerFetchContext::~MbWorkerFetchContext() = default;

void MbWorkerFetchContext::SetTerminateSyncLoadEvent(base::WaitableEvent*) {}

void MbWorkerFetchContext::InitializeOnWorkerThread(
    blink::AcceptLanguagesWatcher*) {}

blink::URLLoaderFactory* MbWorkerFetchContext::GetURLLoaderFactory() {
  return factory_.get();
}

std::unique_ptr<blink::URLLoaderFactory>
MbWorkerFetchContext::WrapURLLoaderFactory(
    blink::CrossVariantMojoRemote<
        network::mojom::URLLoaderFactoryInterfaceBase> /*url_loader_factory*/) {
  // We ignore the passed network-service factory remote and keep routing through
  // libcurl, so a wrapped factory behaves identically to the primary one.
  return std::make_unique<MbWorkerURLLoaderFactory>(user_agent_, extra_headers_,
                                                     view_context_);
}

void MbWorkerFetchContext::FinalizeRequest(blink::WebURLRequest&) {}

std::vector<std::unique_ptr<blink::URLLoaderThrottle>>
MbWorkerFetchContext::CreateThrottles(const network::ResourceRequest&) {
  return {};
}

blink::mojom::ControllerServiceWorkerMode
MbWorkerFetchContext::GetControllerServiceWorkerMode() const {
  return blink::mojom::ControllerServiceWorkerMode::kNoController;
}

net::SiteForCookies MbWorkerFetchContext::SiteForCookies() const {
  return net::SiteForCookies();
}

std::optional<blink::WebSecurityOrigin> MbWorkerFetchContext::TopFrameOrigin()
    const {
  // Must be non-null for a dedicated worker (WorkerFetchContext DCHECKs this). Rebuild
  // from the serialized string on the calling (worker) thread; "null" yields an opaque
  // origin, which is still an engaged optional.
  return blink::WebSecurityOrigin::CreateFromString(blink::WebString::FromUtf8(
      top_frame_origin_.empty() ? std::string("null") : top_frame_origin_));
}

blink::WebString MbWorkerFetchContext::GetAcceptLanguages() const {
  return blink::WebString::FromUtf8("en-US");
}

scoped_refptr<MbWorkerFetchContext> MbWorkerFetchContext::CloneContext() const {
  return base::MakeRefCounted<MbWorkerFetchContext>(user_agent_, extra_headers_,
                                                    top_frame_origin_,
                                                    view_context_);
}

}  // namespace mb
