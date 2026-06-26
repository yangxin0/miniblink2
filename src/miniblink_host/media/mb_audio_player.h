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
#include "base/time/time.h"
#include "base/timer/timer.h"
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
  bool HasVideo() const override { return has_video_; }
  gfx::Size NaturalSize() const override { return natural_size_; }
  double Duration() const override { return duration_; }
  double CurrentTime() const override;
  bool Paused() const override { return paused_; }
  bool IsEnded() const override { return ended_; }
  NetworkState GetNetworkState() const override { return network_state_; }
  ReadyState GetReadyState() const override { return ready_state_; }
  void Play() override;
  void Pause(PauseReason) override;
  void Seek(double seconds) override;
  void SetRate(double rate) override;
  void Shutdown() override {
    play_timer_.Stop();
    weak_ptr_factory_.InvalidateWeakPtrsAndDoom();
  }
  base::WeakPtr<WebMediaPlayer> AsWeakPtr() override {
    return weak_ptr_factory_.GetWeakPtr();
  }

  // --- empty defaults (mirror blink::EmptyWebMediaPlayer) ---
  void SetVolume(double) override {}
  void SetLatencyHint(double) override {}
  void SetPreservesPitch(bool) override {}
  void SetWasPlayedWithUserActivationAndHighMediaEngagement(bool) override {}
  void SetShouldPauseWhenFrameIsHidden(bool) override {}
  void OnRequestPictureInPicture() override {}
  // We have the whole clip in memory, so it is fully buffered AND seekable. An empty
  // seekable range makes the element clamp/refuse currentTime sets (no `seeked`).
  blink::WebTimeRanges Buffered() const override {
    return has_audio_ ? blink::WebTimeRanges(0.0, duration_)
                      : blink::WebTimeRanges();
  }
  blink::WebTimeRanges Seekable() const override {
    return has_audio_ ? blink::WebTimeRanges(0.0, duration_)
                      : blink::WebTimeRanges();
  }
  void OnFrozen() override {}
  bool SetSinkId(const blink::WebString&,
                 blink::WebSetSinkIdCompleteCallback) override {
    return false;
  }
  gfx::Size VisibleSize() const override { return natural_size_; }
  bool Seeking() const override { return seeking_; }
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
  // The playback clock tick (fires TimeChanged -> the element's timeupdate/ended).
  void OnPlaybackTick();

  blink::MediaPlayerClient* client_;  // internal client: state-change callbacks
  scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
  double duration_ = 0.0;
  // `anchor_media_` is the playback position at `anchor_ticks_`; while playing,
  // CurrentTime() = anchor_media_ + (now - anchor_ticks_) * rate_, clamped to duration.
  double anchor_media_ = 0.0;
  base::TimeTicks anchor_ticks_;
  double rate_ = 1.0;
  bool has_audio_ = false;
  bool has_video_ = false;
  gfx::Size natural_size_;  // video dimensions (codec width/height), empty for audio
  bool paused_ = true;
  bool ended_ = false;
  bool seeking_ = false;
  NetworkState network_state_ = kNetworkStateEmpty;
  ReadyState ready_state_ = kReadyStateHaveNothing;
  base::RepeatingTimer play_timer_;  // drives timeupdate/ended while playing
  base::WeakPtrFactory<MbAudioPlayer> weak_ptr_factory_{this};
};

}  // namespace mb

#endif  // MINIBLINK_HOST_MEDIA_MB_AUDIO_PLAYER_H_
