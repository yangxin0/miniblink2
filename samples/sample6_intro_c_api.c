/* Sample 6 — Intro to the C API (Ultralight sample-set parity; macOS + Windows).
 *
 * Compiled as PLAIN C (-std=c99): the whole mb* surface is a flat C ABI —
 * there is no separate CAPI layer to learn, unlike Ultralight's C++-first SDK.
 * Headless: init, load an in-memory document, paint into a malloc'd BGRA
 * buffer, verify a pixel, tear down. No window, no run loop.
 *
 * Run:  ./sample6_intro_c_api
 */
#include <stdio.h>
#include <stdlib.h>

#include "miniblink2/automation.h" /* the embedder core + one-shot paint */

int main(void) {
  const int W = 320, H = 200;

  if (!mbInitialize()) {
    fprintf(stderr, "engine init failed\n");
    return 1;
  }
  mbView* view = mbCreateView(W, H);

  /* An in-memory document: a solid red page. mbLoadHTML pumps until the
   * document's `load` event, so the page is ready when it returns. */
  mbLoadHTML(view,
             "<body style='margin:0;background:#f00'>"
             "<h1 style='color:#fff;font-family:sans-serif'>plain C</h1>"
             "</body>",
             "about:blank");

  /* One-shot capture into caller-owned memory: BGRA8888, premultiplied
   * alpha, sRGB (see the pixel contract at the top of webview.h). */
  int pitch = W * 4;
  unsigned char* pixels = (unsigned char*)malloc((size_t)pitch * H);
  if (!pixels || !mbPaintToBitmap(view, pixels, W, H, pitch)) {
    fprintf(stderr, "paint failed\n");
    return 1;
  }

  /* The page background at the buffer's center must be red: B=0 G=0 R=255. */
  const unsigned char* px = pixels + (H / 2) * pitch + (W / 2) * 4;
  int ok = px[0] < 40 && px[1] < 40 && px[2] > 200;
  printf("center pixel BGRA = (%d, %d, %d, %d) -> %s\n",
         px[0], px[1], px[2], px[3], ok ? "red, as expected" : "WRONG");

  /* Read data back out of the page, C-style: sized out-buffer, full length
   * returned (size first with out=NULL if you need to allocate). */
  char title[64];
  mbEvalJS(view, "document.querySelector('h1').textContent", title, sizeof title);
  printf("h1 text via mbEvalJS = \"%s\"\n", title);

  free(pixels);
  mbDestroyView(view);
  mbShutdown();
  return ok ? 0 : 1;
}
