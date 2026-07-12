#include "miniblink_host/worker/mb_worker_script.h"

#include <stdint.h>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "mojo/public/cpp/system/string_data_source.h"
#include "net/http/http_response_headers.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

#include <vector>

#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "miniblink_host/blob/mb_blob_registry.h"
#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/runtime/mb_runtime.h"
#include "miniblink_host/test/mb_test_seams.h"

namespace mb {
namespace {

// Streams the worker's main script to blink and then self-destructs. blink (the
// WorkerMainScriptLoader) binds the URLLoaderClient end and reads the body data pipe to
// EOF; it finishes only once it has BOTH the pipe EOF and our OnComplete. So we write the
// script bytes, drop the producer (EOF), and push OnComplete. We are also the (inert)
// URLLoader, and tie our lifetime to that receiver: when blink drops the loader remote
// (script consumed), we delete ourselves.
class MbWorkerScript : public network::mojom::URLLoader {
 public:
  MbWorkerScript(mojo::PendingReceiver<network::mojom::URLLoader> loader_receiver,
                 mojo::Remote<network::mojom::URLLoaderClient> client,
                 mojo::ScopedDataPipeProducerHandle producer,
                 std::string body)
      : receiver_(this, std::move(loader_receiver)),
        client_(std::move(client)),
        producer_(std::make_unique<mojo::DataPipeProducer>(std::move(producer))),
        body_(std::move(body)) {
    receiver_.set_disconnect_handler(
        base::BindOnce(&MbWorkerScript::OnDone, base::Unretained(this)));
    producer_->Write(
        std::make_unique<mojo::StringDataSource>(
            base::span<const char>(body_),
            mojo::StringDataSource::AsyncWritingMode::
                STRING_STAYS_VALID_UNTIL_COMPLETION),
        base::BindOnce(&MbWorkerScript::OnBodyWritten, base::Unretained(this)));
  }

  // network::mojom::URLLoader (blink never drives these for a pre-fetched script):
  void FollowRedirect(network::HttpRequestHeadersUpdateParams,
                      const std::optional<GURL>&) override {}
  void SetPriority(net::RequestPriority, int32_t) override {}

 private:
  void OnBodyWritten(MojoResult /*result*/) {
    producer_.reset();  // close the producer -> blink sees end-of-data
    client_->OnComplete(network::URLLoaderCompletionStatus(net::OK));
    // Stay alive until blink drops the loader remote (OnDone deletes us).
  }
  void OnDone() { delete this; }

  mojo::Receiver<network::mojom::URLLoader> receiver_;
  mojo::Remote<network::mojom::URLLoaderClient> client_;
  std::unique_ptr<mojo::DataPipeProducer> producer_;
  std::string body_;  // must outlive the async write (STAYS_VALID_UNTIL_COMPLETION)
};

struct WorkerScriptFetchState {
  base::WaitableEvent done;
  std::atomic<bool> cancelled{false};
  bool ok = false;
  std::string body;
  std::string content_type;
};

base::Lock& WorkerScriptWaiterLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

std::map<std::string, int>& WorkerScriptWaitersForTesting() {
  static auto* waiters = new std::map<std::string, int>();
  return *waiters;
}

class ScopedWorkerScriptWaiterForTesting {
 public:
  explicit ScopedWorkerScriptWaiterForTesting(std::string url)
      : url_(std::move(url)) {
    base::AutoLock guard(WorkerScriptWaiterLock());
    ++WorkerScriptWaitersForTesting()[url_];
  }

  ~ScopedWorkerScriptWaiterForTesting() {
    base::AutoLock guard(WorkerScriptWaiterLock());
    auto it = WorkerScriptWaitersForTesting().find(url_);
    if (it == WorkerScriptWaitersForTesting().end())
      return;
    if (--it->second == 0)
      WorkerScriptWaitersForTesting().erase(it);
  }

 private:
  const std::string url_;
};

void FetchWorkerScriptOnEngine(
    std::string url,
    scoped_refptr<MbLoaderViewContext> view_context,
    std::shared_ptr<WorkerScriptFetchState> state) {
  if (state->cancelled.load(std::memory_order_acquire)) {
    state->done.Signal();
    return;
  }

  const void* host_ctx = view_context ? view_context->host_ctx() : nullptr;
  if (url.rfind("blob:", 0) == 0) {
    // Blob bytes live on the runtime service sequence. The engine sequence may wait for
    // that service reply; worker callers wait on this engine task, never the reverse.
    base::WaitableEvent blob_done;
    std::vector<uint8_t> bytes;
    MbRuntime::ServiceTaskRunner()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](const std::string& u, std::vector<uint8_t>* out,
               base::WaitableEvent* done) {
              MbResolveBlobUrlBytes(
                  u, base::BindOnce(
                         [](std::vector<uint8_t>* out, base::WaitableEvent* done,
                            std::vector<uint8_t> b) {
                           *out = std::move(b);
                           done->Signal();
                         },
                         out, done));
            },
            url, &bytes, &blob_done));
    blob_done.Wait();
    if (!bytes.empty()) {
      state->body.assign(bytes.begin(), bytes.end());
      state->content_type = "text/javascript";
      int status = 200;
      std::string headers;
      MbInvokeResponseHook(host_ctx, url, &status, &headers, &state->body);
      state->ok = true;
    }
  } else {
    state->ok = MbFetchUrl(
        url, &state->body, &state->content_type, /*user_agent=*/"",
        /*extra_headers=*/"", /*post_body=*/"", /*post_content_type=*/"",
        /*http_method=*/"", /*out_final_url=*/nullptr,
        /*out_status=*/nullptr, /*out_headers=*/nullptr, /*out_error=*/nullptr,
        /*out_error_code=*/nullptr, host_ctx, /*run_response_hook=*/true,
        view_context ? view_context->session_key() : std::string());
  }
  state->done.Signal();
}

bool FetchWorkerScript(
    const std::string& url,
    const scoped_refptr<MbLoaderViewContext>& view_context,
    std::string* body,
    std::string* content_type) {
  std::vector<scoped_refptr<MbLoaderViewContext>> activity_contexts;
  if (view_context)
    activity_contexts = view_context->activity_contexts();
  for (const auto& activity_context : activity_contexts)
    MbNetRequestStarted(activity_context->activity_key());
  auto finish_activity = [&] {
    for (const auto& activity_context : activity_contexts)
      MbNetRequestFinished(activity_context->activity_key());
  };

  auto state = std::make_shared<WorkerScriptFetchState>();
  const scoped_refptr<base::SingleThreadTaskRunner> engine_runner =
      view_context ? view_context->engine_task_runner() : nullptr;
  if (!engine_runner || engine_runner->RunsTasksInCurrentSequence()) {
    FetchWorkerScriptOnEngine(url, view_context, state);
  } else {
    if (!engine_runner->PostTask(
            FROM_HERE,
            base::BindOnce(&FetchWorkerScriptOnEngine, url, view_context,
                           state))) {
      finish_activity();
      return false;
    }
    std::optional<ScopedWorkerScriptWaiterForTesting> waiter;
    while (!state->done.TimedWait(base::Milliseconds(10))) {
      // Publish only once this worker has genuinely waited for the engine task.
      // That makes teardown tests distinguish the rendezvous from a merely
      // queued worker creation request.
      if (!waiter)
        waiter.emplace(url);
      // Destroying the creating view can synchronously terminate/join this worker.
      // Abandon the rendezvous rather than deadlocking teardown; the posted task owns
      // its state and observes cancellation before starting if it runs later. Same
      // escape once mbShutdown has begun: the engine loop is never pumped again.
      if ((view_context && !view_context->is_alive()) ||
          mb::MbRuntime::ShutdownStarted()) {
        state->cancelled.store(true, std::memory_order_release);
        finish_activity();
        return false;
      }
    }
  }

  finish_activity();
  if (!state->ok)
    return false;
  *body = std::move(state->body);
  *content_type = std::move(state->content_type);
  return true;
}

}  // namespace

int MbWorkerScriptWaiterCountForTesting(const std::string& url) {
  base::AutoLock guard(WorkerScriptWaiterLock());
  auto it = WorkerScriptWaitersForTesting().find(url);
  return it == WorkerScriptWaitersForTesting().end() ? 0 : it->second;
}

std::unique_ptr<blink::WorkerMainScriptLoadParameters> MakeWorkerMainScriptParams(
    const std::string& url,
    scoped_refptr<MbLoaderViewContext> view_context) {
  std::string body, content_type;
  if (!FetchWorkerScript(url, view_context, &body, &content_type))
    return nullptr;
  std::string mime = content_type.substr(0, content_type.find(';'));
  if (mime.empty())
    mime = "text/javascript";

  auto params = std::make_unique<blink::WorkerMainScriptLoadParameters>();
  params->request_id = 1;
  params->response_head = network::mojom::URLResponseHead::New();
  params->response_head->mime_type = mime;
  // A Content-Type HEADER (not just the mime_type field) is required for MODULE workers:
  // WorkerModuleScriptFetcher reads ResourceResponse::HttpContentType() and enforces a
  // JavaScript MIME type. Classic workers don't check, but it's harmless for them.
  params->response_head->headers = net::HttpResponseHeaders::TryToCreate(
      "HTTP/1.1 200 OK\nContent-Type: " + mime + "\n\n");

  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  if (mojo::CreateDataPipe(nullptr, producer, consumer) != MOJO_RESULT_OK)
    return nullptr;
  params->response_body = std::move(consumer);

  mojo::PendingRemote<network::mojom::URLLoader> loader_remote;
  auto loader_receiver = loader_remote.InitWithNewPipeAndPassReceiver();
  mojo::Remote<network::mojom::URLLoaderClient> client_remote;
  auto client_receiver = client_remote.BindNewPipeAndPassReceiver();
  params->url_loader_client_endpoints =
      network::mojom::URLLoaderClientEndpoints::New(std::move(loader_remote),
                                                    std::move(client_receiver));
  // Owns its endpoints; deletes itself when blink finishes consuming the script.
  new MbWorkerScript(std::move(loader_receiver), std::move(client_remote),
                     std::move(producer), std::move(body));
  return params;
}

}  // namespace mb
