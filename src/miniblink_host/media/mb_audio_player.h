// mb_audio_player — a minimal audio-only blink::WebMediaPlayer (media bring-up step 1).
//
// blink's HTMLMediaElement creates a WebMediaPlayer via WebLocalFrameClient::Create-
// MediaPlayer; base returns null, so <audio>/<video> have no player and never load.
// This player handles AUDIO: on Load it fetches the src (MbFetchUrl: data:/file/http)
// and decodes it with FFmpeg (media::AudioFileReader, the decodeAudioData path), then
// reports duration + reaches HAVE_ENOUGH_DATA so an <audio> element fires
// loadedmetadata / durationchange / canplaythrough and audio.duration is correct.
//
// Scope: METADATA + decode (step 1). A real playback timeline (currentTime advancing,
// play/pause/ended, audio output) is a follow-on. The element reports state by POLLING
// the player (GetReadyState/GetNetworkState/Duration) after each client notification;
// the notifications go through blink's internal MediaPlayerClient (we link blink
// internals). All non-audio methods mirror blink::EmptyWebMediaPlayer's empty defaults.

#ifndef MINIBLINK_HOST_MEDIA_MB_AUDIO_PLAYER_H_
#define MINIBLINK_HOST_MEDIA_MB_AUDIO_PLAYER_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "media/base/picture_in_picture_events_info.h"
#include "third_party/blink/public/platform/web_media_player.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_time_range.h"
#include "ui/gfx/geometry/size.h"

namespace cc {
class PaintCanvas;
class PaintFlags;
}  // namespace cc

namespace blink {
class MediaPlayerClient;
class WebMediaPlayerClient;
class WebMediaPlayerSource;
}  // namespace blink

namespace mb {

class MbAudioPlayer : public blink::WebMediaPlayer {
 public:
  MbAudioPlayer(blink::WebMediaPlayerClient* client,
                scoped_refptr<base::SingleThreadTaskRunner> task_runner);
  ~MbAudioPlayer() override;

  // --- the methods we actually implement (audio metadata + decode) ---
  LoadTiming Load(LoadType,
                  const blink::WebMediaPlayerSource&,
                  CorsMode,
                  bool is_cache_disabled) override;
  bool HasAudio() const override { return has_audio_; }
  bool HasVideo() const override { return false; }
  double Duration() const override { return duration_; }
  double CurrentTime() const override { return current_time_; }
  bool Paused() const override { return paused_; }
  NetworkState GetNetworkState() const override { return network_state_; }
  ReadyState GetReadyState() const override { return ready_state_; }
  void Play() override { paused_ = false; }
  void Pause(PauseReason) override { paused_ = true; }
  void Shutdown() override { weak_ptr_factory_.InvalidateWeakPtrsAndDoom(); }
  base::WeakPtr<WebMediaPlayer> AsWeakPtr() override {
    return weak_ptr_factory_.GetWeakPtr();
  }

  // --- empty defaults (mirror blink::EmptyWebMediaPlayer) ---
  void Seek(double) override {}
  void SetRate(double) override {}
  void SetVolume(double) override {}
  void SetLatencyHint(double) override {}
  void SetPreservesPitch(bool) override {}
  void SetWasPlayedWithUserActivationAndHighMediaEngagement(bool) override {}
  void SetShouldPauseWhenFrameIsHidden(bool) override {}
  void OnRequestPictureInPicture() override {}
  blink::WebTimeRanges Buffered() const override {
    return blink::WebTimeRanges();
  }
  blink::WebTimeRanges Seekable() const override {
    return blink::WebTimeRanges();
  }
  void OnFrozen() override {}
  bool SetSinkId(const blink::WebString&,
                 blink::WebSetSinkIdCompleteCallback) override {
    return false;
  }
  gfx::Size NaturalSize() const override { return gfx::Size(); }
  gfx::Size VisibleSize() const override { return gfx::Size(); }
  bool Seeking() const override { return false; }
  bool IsEnded() const override { return false; }
  blink::WebString GetErrorMessage() const override {
    return blink::WebString();
  }
  bool DidLoadingProgress() override { return false; }
  bool WouldTaintOrigin() const override { return false; }
  double MediaTimeForTimeValue(double time_value) const override {
    return time_value;
  }
  unsigned DecodedFrameCount() const override { return 0; }
  unsigned DroppedFrameCount() const override { return 0; }
  uint64_t AudioDecodedByteCount() const override { return 0; }
  uint64_t VideoDecodedByteCount() const override { return 0; }
  void SetVolumeMultiplier(double) override {}
  void SetPowerExperimentState(bool) override {}
  void SuspendForFrameClosed() override {}
  void RecordAutoPictureInPictureInfo(
      const media::PictureInPictureEventsInfo::AutoPipInfo&) override {}
  void Paint(cc::PaintCanvas*,
             const gfx::Rect&,
             const cc::PaintFlags&,
             bool) override {}
  scoped_refptr<media::VideoFrame> GetCurrentFrameThenUpdate() override {
    return nullptr;
  }
  std::optional<media::VideoFrame::ID> CurrentFrameId() const override {
    return std::nullopt;
  }
  bool HasAvailableVideoFrame() const override { return false; }
  bool HasReadableVideoFrame() const override { return false; }
  void RegisterFrameSinkHierarchy() override {}
  void UnregisterFrameSinkHierarchy() override {}

 private:
  // Fetch `url`, decode it, set state, and notify the element (runs off Load()).
  void DecodeAndReport(std::string url);

  blink::MediaPlayerClient* client_;  // internal client: state-change callbacks
  scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
  double duration_ = 0.0;
  double current_time_ = 0.0;
  bool has_audio_ = false;
  bool paused_ = true;
  NetworkState network_state_ = kNetworkStateEmpty;
  ReadyState ready_state_ = kReadyStateHaveNothing;
  base::WeakPtrFactory<MbAudioPlayer> weak_ptr_factory_{this};
};

}  // namespace mb

#endif  // MINIBLINK_HOST_MEDIA_MB_AUDIO_PLAYER_H_
