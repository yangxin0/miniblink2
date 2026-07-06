// One CDP session bridged flat to the host (IMPROVEMENT.md item 13c, stage A).
// Attach binds blink's in-binary DevToolsAgent (the frame-owned agent) over
// in-process mojo endpoints; Send dispatches one client CDP command (JSON with
// "id"/"method"); responses and notifications arrive on the callback as CDP
// JSON. Everything runs on the embedder main thread - delivery rides the task
// queue, so the host must keep pumping (mbUpdate) for messages to flow.
// Interrupt-class commands (Debugger.pause, Runtime.terminateExecution, ...)
// are routed via the IO session so they reach V8 even while script is running.

#ifndef MINIBLINK_HOST_DEVTOOLS_MB_DEVTOOLS_H_
#define MINIBLINK_HOST_DEVTOOLS_MB_DEVTOOLS_H_

#include <functional>
#include <memory>
#include <string>

namespace blink {
class WebLocalFrameImpl;
}

namespace mb {

class MbDevToolsBridge {
 public:
  using MessageCallback = std::function<void(const std::string&)>;

  static std::unique_ptr<MbDevToolsBridge> Attach(blink::WebLocalFrameImpl*,
                                                  MessageCallback on_message);
  virtual ~MbDevToolsBridge() = default;
  virtual void Send(const std::string& json) = 0;
};

}  // namespace mb

#endif  // MINIBLINK_HOST_DEVTOOLS_MB_DEVTOOLS_H_
