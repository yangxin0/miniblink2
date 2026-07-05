#include "miniblink_host/devtools/mb_devtools.h"

#include <algorithm>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/task/single_thread_task_runner.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/mojom/devtools/devtools_agent.mojom-blink.h"
#include "third_party/blink/renderer/core/exported/web_dev_tools_agent_impl.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/inspector_protocol/crdtp/dispatch.h"
#include "third_party/inspector_protocol/crdtp/json.h"
#include "third_party/inspector_protocol/crdtp/span.h"
#include "third_party/inspector_protocol/crdtp/status.h"

namespace mb {
namespace {

// Interrupt-class commands must reach V8 even while the main thread is stuck
// in script, so they go on the IO session: its receiver lives on the Platform
// IO thread and dispatches through InspectorTaskRunner with a V8 isolate
// interrupt. Keep in sync with content::DevToolsSession::ShouldSendOnIO.
bool ShouldSendOnIO(crdtp::span<uint8_t> method) {
  static auto* kEntries = new std::vector<crdtp::span<uint8_t>>{
      crdtp::SpanFrom("Debugger.getPossibleBreakpoints"),
      crdtp::SpanFrom("Debugger.getScriptSource"),
      crdtp::SpanFrom("Debugger.getStackTrace"),
      crdtp::SpanFrom("Debugger.pause"),
      crdtp::SpanFrom("Debugger.removeBreakpoint"),
      crdtp::SpanFrom("Debugger.resume"),
      crdtp::SpanFrom("Debugger.setBreakpoint"),
      crdtp::SpanFrom("Debugger.setBreakpointByUrl"),
      crdtp::SpanFrom("Debugger.setBreakpointsActive"),
      crdtp::SpanFrom("Emulation.setScriptExecutionDisabled"),
      crdtp::SpanFrom("Page.crash"),
      crdtp::SpanFrom("Performance.getMetrics"),
      crdtp::SpanFrom("Runtime.terminateExecution"),
  };
  DCHECK(std::is_sorted(kEntries->begin(), kEntries->end(), crdtp::SpanLt()));
  return std::binary_search(kEntries->begin(), kEntries->end(), method,
                            crdtp::SpanLt());
}

class BridgeImpl : public MbDevToolsBridge,
                   public blink::mojom::blink::DevToolsSessionHost,
                   public blink::mojom::blink::DevToolsAgentHost {
 public:
  BridgeImpl(blink::WebDevToolsAgentImpl* agent, MessageCallback on_message)
      : on_message_(std::move(on_message)), web_agent_(agent) {
    // The agent/host at BindReceiver are the association ROOT: a fresh
    // AssociatedRemote/Receiver's BindNewEndpointAndPass* creates a dedicated
    // primary pipe (the standalone-embedder equivalent of the frame's
    // browser<->renderer channel).
    web_agent_->GetDevToolsAgent()->BindReceiverForWorker(
        agent_host_receiver_.BindNewPipeAndPassRemote(),
        agent_.BindNewPipeAndPassReceiver(),
        base::SingleThreadTaskRunner::GetCurrentDefault());
    // The session endpoints MUST be non-dedicated: passed as associated params
    // of AttachDevToolsSession (a message on agent_), they associate with
    // agent_'s group. Dedicated endpoints belong to a foreign group, so the
    // message carried mismatched-group params and mojo silently dropped it -
    // which is why the agent never received AttachDevToolsSession.
    agent_->AttachDevToolsSession(
        session_host_receiver_.BindNewEndpointAndPassRemote(),
        session_.BindNewEndpointAndPassReceiver(),
        io_session_.BindNewPipeAndPassReceiver(),
        /*reattach_session_state=*/nullptr,
        /*script_to_evaluate_on_load=*/blink::g_empty_string,
        /*client_expects_binary_responses=*/false,
        /*client_is_trusted=*/true,
        // Empty on purpose: blink only stamps "sessionId" into response and
        // notification envelopes when this is non-empty, and a DIRECTLY
        // connected frontend (Chrome speaking to the ws endpoint without
        // Target.attachToTarget) drops events tagged with a session it never
        // created - responses still match by call id, so the inspector half
        // works: DOM skeleton but no setChildNodes/scriptParsed/console
        // events. No flat-mode routing happens on this single-session pipe,
        // so the id adds nothing.
        /*session_id=*/blink::g_empty_string,
        /*session_waits_for_debugger=*/false);
  }

  ~BridgeImpl() override {
    if (web_agent_)
      web_agent_->FlushProtocolNotifications();
    // Closing our endpoints detaches the session asynchronously: blink's
    // DevToolsSession::Detach is a mojo disconnect handler and runs on the
    // next pump. The agent itself is frame-owned (see Attach), so frame
    // teardown disposes whatever a never-pumped session leaves behind.
  }

  void Send(const std::string& json) override {
    if (!session_)
      return;
    // The session requires a CBOR-encoded command (DispatchProtocolCommandImpl
    // DCHECKs IsCBORMessage); transcode the client's JSON. Responses stay JSON
    // (client_expects_binary_responses=false), delivered via Forward().
    std::vector<uint8_t> cbor;
    crdtp::Status st = crdtp::json::ConvertJSONToCBOR(
        crdtp::span<uint8_t>(reinterpret_cast<const uint8_t*>(json.data()),
                             json.size()),
        &cbor);
    if (!st.ok())
      return;
    // Shallow-parse the top-level routing fields ("id"/"method") from the
    // CBOR. blink's DevToolsSession assumes a pre-validated command with an
    // integer id (it DCHECKs Dispatchable::ok()); there is no browser-side
    // validation layer in this embedder, so reject malformed input here.
    crdtp::Dispatchable dispatchable(
        crdtp::span<uint8_t>(cbor.data(), cbor.size()), std::string_view(),
        /*fallthrough_callback=*/nullptr);
    if (!dispatchable.ok() || !dispatchable.HasCallId())
      return;
    blink::String method(std::string(
        reinterpret_cast<const char*>(dispatchable.Method().data()),
        dispatchable.Method().size()));
    base::span<const uint8_t> command(cbor.data(), cbor.size());
    if (ShouldSendOnIO(dispatchable.Method())) {
      io_session_->DispatchProtocolCommand(dispatchable.CallId(), method,
                                           command, blink::g_empty_string);
    } else {
      session_->DispatchProtocolCommand(dispatchable.CallId(), method, command,
                                        blink::g_empty_string);
    }
  }

  // blink::mojom::blink::DevToolsSessionHost:
  void DispatchProtocolResponse(
      blink::mojom::blink::DevToolsMessagePtr message,
      int32_t call_id,
      blink::mojom::blink::RendererOriginatingSessionStatePtr) override {
    Forward(*message);
  }
  void DispatchProtocolNotification(
      blink::mojom::blink::DevToolsMessagePtr message,
      blink::mojom::blink::RendererOriginatingSessionStatePtr) override {
    Forward(*message);
  }

  // blink::mojom::blink::DevToolsAgentHost: single-target v1 - child worker
  // targets and window management are not surfaced.
  void ChildTargetCreated(
      mojo::PendingRemote<blink::mojom::blink::DevToolsAgent>,
      mojo::PendingReceiver<blink::mojom::blink::DevToolsAgentHost>,
      const blink::KURL&,
      const blink::String&,
      const base::UnguessableToken&,
      bool,
      blink::mojom::blink::DevToolsExecutionContextType) override {}
  void MainThreadDebuggerPaused() override {}
  void MainThreadDebuggerResumed() override {}
  void BringToForeground() override {}

 private:
  void Forward(const blink::mojom::blink::DevToolsMessage& message) {
    if (!on_message_)
      return;
    on_message_(std::string(
        reinterpret_cast<const char*>(message.data.data()),
        message.data.size()));
  }

  MessageCallback on_message_;
  blink::Persistent<blink::WebDevToolsAgentImpl> web_agent_;
  mojo::Remote<blink::mojom::blink::DevToolsAgent> agent_;
  mojo::AssociatedRemote<blink::mojom::blink::DevToolsSession> session_;
  // Interrupt-class command path (ShouldSendOnIO). Even if never used it must
  // stay bound: its disconnect handler on the agent side detaches the session.
  mojo::Remote<blink::mojom::blink::DevToolsSession> io_session_;
  mojo::AssociatedReceiver<blink::mojom::blink::DevToolsSessionHost>
      session_host_receiver_{this};
  mojo::Receiver<blink::mojom::blink::DevToolsAgentHost>
      agent_host_receiver_{this};
};

}  // namespace

// static
std::unique_ptr<MbDevToolsBridge> MbDevToolsBridge::Attach(
    blink::WebLocalFrameImpl* frame,
    MessageCallback on_message) {
  if (!frame)
    return nullptr;
  // Use the frame-owned agent (WebLocalFrameImpl::dev_tools_agent_), never a
  // free-floating CreateForFrame instance: overlay painting, disposal at
  // WillBeDetached, and worker auto-attach all key on that member and would
  // bypass an unregistered agent.
  blink::WebDevToolsAgentImpl* agent =
      frame->DevToolsAgentImpl(/*create_if_necessary=*/true);
  if (!agent)
    return nullptr;
  return std::make_unique<BridgeImpl>(agent, std::move(on_message));
}

}  // namespace mb
