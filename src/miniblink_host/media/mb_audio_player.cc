#include "miniblink_host/media/mb_audio_player.h"

#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "media/filters/demuxer_manager.h"  // media::TrackManager (a MediaPlayerClient base)
#include "media/filters/ffmpeg_glue.h"
#include "media/filters/in_memory_url_protocol.h"
#include "miniblink_host/loader/mb_url_loader.h"
#include "third_party/blink/public/platform/web_media_player_source.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/renderer/platform/media/media_player_client.h"
#include "ui/gfx/geometry/size.h"

// FFmpeg headers MUST come last: media/ffmpeg/ffmpeg_common.h is visibility-restricted to
// //media, so we include libav* directly (//third_party/ffmpeg supplies the path), and its
// macros would otherwise pollute the blink/chromium headers above.
extern "C" {
#include <libavformat/avformat.h>
}

namespace mb {

MbAudioPlayer::MbAudioPlayer(
    blink::WebMediaPlayerClient* client,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner)
    // The public WebMediaPlayerClient blink hands us IS its internal MediaPlayerClient
    // (the only subclass — see web_media_player_client.h's friend); downcast so we can
    // drive the state-change callbacks (NetworkStateChanged/ReadyStateChanged/...).
    : client_(static_cast<blink::MediaPlayerClient*>(client)),
      task_runner_(std::move(task_runner)) {}

MbAudioPlayer::~MbAudioPlayer() = default;

blink::WebMediaPlayer::LoadTiming MbAudioPlayer::Load(
    LoadType,
    const blink::WebMediaPlayerSource& source,
    CorsMode,
    bool /*is_cache_disabled*/) {
  network_state_ = kNetworkStateLoading;
  std::string url = source.IsURL() ? source.GetAsURL().GetString().Utf8() : "";
  // Fetch + decode + notify off the Load() call so we never notify the element
  // re-entrantly (it polls our state after each callback).
  task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&MbAudioPlayer::DecodeAndReport,
                                weak_ptr_factory_.GetWeakPtr(), std::move(url)));
  return LoadTiming::kDeferred;
}

void MbAudioPlayer::DecodeAndReport(std::string url) {
  std::string body, content_type;
  bool ok = !url.empty() && MbFetchUrl(url, &body, &content_type);
  if (ok) {
    // Read container metadata with FFmpeg (no decode): stream kinds + video size +
    // duration. avformat_find_stream_info fills codecpar->width/height without a
    // decoder, so this covers <audio> AND <video> (and audio/video tracks together).
    media::InMemoryUrlProtocol protocol(base::as_byte_span(body),
                                        /*streaming=*/false);
    media::FFmpegGlue glue(&protocol);
    if (glue.OpenContext() &&
        avformat_find_stream_info(glue.format_context(), nullptr) >= 0) {
      AVFormatContext* fc = glue.format_context();
      for (unsigned i = 0; i < fc->nb_streams; ++i) {
        const AVCodecParameters* cp = fc->streams[i]->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
          has_audio_ = true;
        } else if (cp->codec_type == AVMEDIA_TYPE_VIDEO && cp->width > 0 &&
                   cp->height > 0) {
          has_video_ = true;
          natural_size_ = gfx::Size(cp->width, cp->height);
        }
      }
      if (fc->duration != AV_NOPTS_VALUE)
        duration_ = fc->duration / static_cast<double>(AV_TIME_BASE);
      ok = has_audio_ || has_video_;
    } else {
      ok = false;
    }
  }

  if (ok) {
    // Jump straight to "have enough data": the element fires loadedmetadata ->
    // durationchange -> canplay -> canplaythrough for the levels crossed.
    network_state_ = kNetworkStateLoaded;
    ready_state_ = kReadyStateHaveEnoughData;
    client_->DurationChanged();
    client_->ReadyStateChanged();
    client_->NetworkStateChanged();
  } else {
    network_state_ = kNetworkStateFormatError;
    client_->NetworkStateChanged();
  }
}

double MbAudioPlayer::CurrentTime() const {
  if (paused_)
    return anchor_media_;
  double t = anchor_media_ +
             (base::TimeTicks::Now() - anchor_ticks_).InSecondsF() * rate_;
  if (t < 0.0)
    t = 0.0;
  if (t > duration_)
    t = duration_;
  return t;
}

void MbAudioPlayer::Play() {
  if (!has_audio_ && !has_video_)
    return;
  if (ended_) {  // replay from the start
    ended_ = false;
    anchor_media_ = 0.0;
  } else {
    anchor_media_ = CurrentTime();
  }
  anchor_ticks_ = base::TimeTicks::Now();
  paused_ = false;
  // ~30 Hz: advances currentTime + drives the element's timeupdate/ended. There is no
  // real audio output (the WebAudioDevice is silent), but the timeline is real.
  play_timer_.Start(FROM_HERE, base::Milliseconds(33), this,
                    &MbAudioPlayer::OnPlaybackTick);
}

void MbAudioPlayer::Pause(PauseReason) {
  if (paused_)
    return;
  anchor_media_ = CurrentTime();
  paused_ = true;
  play_timer_.Stop();
}

void MbAudioPlayer::Seek(double seconds) {
  if (seconds < 0.0)
    seconds = 0.0;
  if (seconds > duration_)
    seconds = duration_;
  anchor_media_ = seconds;
  anchor_ticks_ = base::TimeTicks::Now();
  ended_ = false;
  seeking_ = true;
  // The element calls Seek() synchronously from its own seek(); notify the completion
  // asynchronously (not reentrantly) so it can finish the seek and fire `seeked`.
  task_runner_->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<MbAudioPlayer> self) {
                       if (!self)
                         return;
                       self->seeking_ = false;
                       self->client_->TimeChanged();
                     },
                     weak_ptr_factory_.GetWeakPtr()));
}

void MbAudioPlayer::SetRate(double rate) {
  anchor_media_ = CurrentTime();  // re-anchor before changing the slope
  anchor_ticks_ = base::TimeTicks::Now();
  rate_ = rate;
}

void MbAudioPlayer::OnPlaybackTick() {
  if (CurrentTime() >= duration_) {
    anchor_media_ = duration_;
    paused_ = true;
    ended_ = true;
    play_timer_.Stop();
  }
  client_->TimeChanged();  // element fires timeupdate, and ended when IsEnded()
}

}  // namespace mb
