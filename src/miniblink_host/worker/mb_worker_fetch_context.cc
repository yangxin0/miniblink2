#include "miniblink_host/worker/mb_worker_fetch_context.h"

#include <utility>

#include "miniblink_host/loader/mb_url_loader.h"

namespace mb {

MbWorkerURLLoaderFactory::MbWorkerURLLoaderFactory(std::string user_agent,
                                                   std::string extra_headers)
    : user_agent_(std::move(user_agent)),
      extra_headers_(std::move(extra_headers)) {}

MbWorkerURLLoaderFactory::~MbWorkerURLLoaderFactory() = default;

std::unique_ptr<blink::URLLoader> MbWorkerURLLoaderFactory::CreateURLLoader(
    const network::ResourceRequest& /*request*/,
    scoped_refptr<base::SingleThreadTaskRunner> /*freezable_task_runner*/,
    scoped_refptr<base::SingleThreadTaskRunner> /*unfreezable_task_runner*/,
    mojo::PendingRemote<blink::mojom::blink::KeepAliveHandle> /*keep_alive*/,
    blink::BackForwardCacheLoaderHelper* /*bf_cache_loader_helper*/,
    blink::Vector<std::unique_ptr<blink::URLLoaderThrottle>> /*throttles*/) {
  // Same libcurl-backed loader the frame uses for subresources.
  return std::make_unique<MbURLLoader>(user_agent_, extra_headers_);
}

MbWorkerFetchContext::MbWorkerFetchContext(std::string user_agent,
                                           std::string extra_headers)
    : user_agent_(std::move(user_agent)),
      extra_headers_(std::move(extra_headers)),
      factory_(std::make_unique<MbWorkerURLLoaderFactory>(user_agent_,
                                                          extra_headers_)) {}

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
  return std::make_unique<MbWorkerURLLoaderFactory>(user_agent_, extra_headers_);
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
  // Unset: the host doesn't model a top-frame origin for the worker. Cookie
  // isolation falls back to the request URL's own origin.
  return std::nullopt;
}

blink::WebString MbWorkerFetchContext::GetAcceptLanguages() const {
  return blink::WebString::FromUtf8("en-US");
}

}  // namespace mb
