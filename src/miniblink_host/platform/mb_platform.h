// mb_platform — the real (non-test) blink::Platform for miniblink-modern.
//
// Modeled on third_party/blink/renderer/platform/testing/testing_platform_support.h
// (see vendor/reference/). KEY FINDING: the base blink::Platform provides default
// impls for almost everything; TestingPlatformSupport overrides only ~6 methods.
// We override the same minimal set, with REAL behavior instead of test stubs.
//
// Status: Phase 1 scaffold. Methods are filled lazily — run, let Blink CHECK-fail
// on what it actually needs, implement that. Do NOT pre-implement the whole surface.

#ifndef MINIBLINK_HOST_PLATFORM_MB_PLATFORM_H_
#define MINIBLINK_HOST_PLATFORM_MB_PLATFORM_H_

#include <memory>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "third_party/blink/public/platform/platform.h"

namespace mb {

class MbPlatform : public blink::Platform {
 public:
  MbPlatform();
  MbPlatform(const MbPlatform&) = delete;
  MbPlatform& operator=(const MbPlatform&) = delete;
  ~MbPlatform() override;

  // blink::Platform — the minimal override set (mirrors TestingPlatformSupport):
  blink::WebString DefaultLocale() override;
  blink::ThreadSafeBrowserInterfaceBrokerProxy* GetBrowserInterfaceBroker()
      override;
  bool IsThreadedAnimationEnabled() override;

  // Resource bundle: Blink asks for built-in resources (UA stylesheet, etc.).
  // P1: back with a real bundle / packed file. TODO(mb): wire resource pak.
  blink::WebData GetDataResource(
      int resource_id,
      ui::ResourceScaleFactor scale_factor) override;
  std::string GetDataResourceString(int resource_id) override;

  // Fonts / sandbox / codecs etc. are defaulted by base Platform on macOS via
  // skia + system; override here only when a CHECK tells us to.

 private:
  // Empty broker: GetInterfaceImpl drops every receiver. Blink core init
  // (TimeZoneController etc.) calls GetInterface() during startup and crashes on a
  // null broker, so this must be a real ref-counted object, not nullptr.
  scoped_refptr<blink::ThreadSafeBrowserInterfaceBrokerProxy> broker_;
};

}  // namespace mb

#endif  // MINIBLINK_HOST_PLATFORM_MB_PLATFORM_H_
