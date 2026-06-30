// mb_media_player — wires Chromium's REAL blink::WebMediaPlayerImpl (full MSE pipeline +
// software VP9/Opus/AV1 decode + RendererImpl A/V sync) so <video> actually plays
// (YouTube). This replaces the audio-only MbAudioPlayer for the playback path.
//
// Step 1 (this file): video frames render through the existing software paint path
// (HTMLVideoElement::PaintCurrentFrame -> WebMediaPlayer::Paint -> PaintCanvasVideoRenderer,
// already driven by MbWebView's kOmitCompositingInfo capture), with a media::NullAudioSink
// — silent, but it provides the real-time clock so playback advances at correct speed.
// Step 2 swaps the NullAudioSink for a real CoreAudio output sink to get sound.

#ifndef MINIBLINK_HOST_MEDIA_MB_MEDIA_PLAYER_H_
#define MINIBLINK_HOST_MEDIA_MB_MEDIA_PLAYER_H_

#include <memory>

namespace blink {
class WebLocalFrame;
class WebMediaPlayer;
class WebMediaPlayerBuilder;
class WebMediaPlayerClient;
}  // namespace blink

namespace mb {

// Build a real blink::WebMediaPlayerImpl for `client` on `frame`. Returns nullptr if media
// support can't initialize, so the caller can fall back to the audio-only MbAudioPlayer.
//
// `builder` MUST outlive the returned player: it owns the UrlIndex (media resource cache)
// that the player holds by raw pointer and dereferences on a later (async) DoLoad. The
// caller (MbFrameClient) keeps one builder per frame, mirroring content's RenderFrameImpl.
std::unique_ptr<blink::WebMediaPlayer> MbCreateWebMediaPlayer(
    blink::WebMediaPlayerBuilder& builder,
    blink::WebLocalFrame* frame,
    blink::WebMediaPlayerClient* client);

}  // namespace mb

#endif  // MINIBLINK_HOST_MEDIA_MB_MEDIA_PLAYER_H_
