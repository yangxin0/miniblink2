#include "miniblink_host/media/mb_audio_player.h"

#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "media/base/encryption_scheme.h"
#include "media/base/video_codecs.h"
#include "media/base/video_color_space.h"
#include "media/base/video_decoder_config.h"
#include "media/base/video_frame.h"
#include "media/base/video_thumbnail_decoder.h"
#include "media/base/video_transformation.h"
#include "media/renderers/paint_canvas_video_renderer.h"
#include "media/filters/demuxer_manager.h"  // media::TrackManager (a MediaPlayerClient base)
#include "media/filters/ffmpeg_glue.h"
#include "media/filters/in_memory_url_protocol.h"
#include "media/filters/vpx_video_decoder.h"
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

  std::vector<uint8_t> video_keyframe;  // first video packet bytes (a keyframe)
  std::vector<uint8_t> video_extra_data;
  media::VideoCodec video_codec = media::VideoCodec::kUnknown;
  media::VideoCodecProfile video_profile = media::VIDEO_CODEC_PROFILE_UNKNOWN;

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
      int video_idx = -1;
      for (unsigned i = 0; i < fc->nb_streams; ++i) {
        const AVCodecParameters* cp = fc->streams[i]->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
          has_audio_ = true;
        } else if (cp->codec_type == AVMEDIA_TYPE_VIDEO && cp->width > 0 &&
                   cp->height > 0) {
          has_video_ = true;
          natural_size_ = gfx::Size(cp->width, cp->height);
          video_idx = static_cast<int>(i);
          if (cp->codec_id == AV_CODEC_ID_VP8) {
            video_codec = media::VideoCodec::kVP8;
            video_profile = media::VP8PROFILE_ANY;
          } else if (cp->codec_id == AV_CODEC_ID_VP9) {
            video_codec = media::VideoCodec::kVP9;
            video_profile = media::VP9PROFILE_PROFILE0;
          }
          if (cp->extradata && cp->extradata_size > 0) {
            video_extra_data.assign(cp->extradata,
                                    cp->extradata + cp->extradata_size);
          }
        }
      }
      if (fc->duration != AV_NOPTS_VALUE)
        duration_ = fc->duration / static_cast<double>(AV_TIME_BASE);
      // Demux the first video packet (a keyframe in our assets) for a one-shot decode.
      if (video_idx >= 0 && video_codec != media::VideoCodec::kUnknown) {
        AVPacket* pkt = av_packet_alloc();
        while (pkt && av_read_frame(fc, pkt) >= 0) {
          if (pkt->stream_index == video_idx && pkt->size > 0) {
            video_keyframe.assign(pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
            break;
          }
          av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
      }
      ok = has_audio_ || has_video_;
    } else {
      ok = false;
    }
  }

  if (!ok) {
    network_state_ = kNetworkStateFormatError;
    client_->NetworkStateChanged();
    return;
  }

  network_state_ = kNetworkStateLoaded;
  if (has_video_ && !video_keyframe.empty()) {
    // Kick off a one-shot decode of the first frame (libvpx). The frame arrives
    // asynchronously -> OnFirstFrameDecoded advances readyState to HAVE_ENOUGH_DATA.
    // Report metadata now (loadedmetadata) at HAVE_METADATA.
    media::VideoDecoderConfig config(
        video_codec, video_profile,
        media::VideoDecoderConfig::AlphaMode::kIsOpaque,
        media::VideoColorSpace::REC709(), media::kNoTransformation, natural_size_,
        gfx::Rect(natural_size_), natural_size_, video_extra_data,
        media::EncryptionScheme::kUnencrypted);
    frame_decoder_ = std::make_unique<media::VideoThumbnailDecoder>(
        std::make_unique<media::VpxVideoDecoder>(), config,
        std::move(video_keyframe));
    frame_decoder_->Start(base::BindOnce(&MbAudioPlayer::OnFirstFrameDecoded,
                                         weak_ptr_factory_.GetWeakPtr()));
    ready_state_ = kReadyStateHaveMetadata;
  } else {
    // Audio-only (or unsupported video codec): jump to "have enough data".
    ready_state_ = kReadyStateHaveEnoughData;
  }
  client_->DurationChanged();
  client_->ReadyStateChanged();
  client_->NetworkStateChanged();
}

void MbAudioPlayer::Paint(cc::PaintCanvas* canvas,
                          const gfx::Rect& rect,
                          const cc::PaintFlags& flags,
                          bool /*force_pixel_readback*/) {
  if (!current_frame_ || !canvas)
    return;
  if (!video_renderer_)
    video_renderer_ = std::make_unique<media::PaintCanvasVideoRenderer>();
  media::PaintCanvasVideoRenderer::PaintParams params;
  params.dest_rect = gfx::RectF(rect);
  // Software (I420) frame -> no raster context needed; PaintCanvasVideoRenderer
  // converts YUV to the canvas.
  video_renderer_->Paint(current_frame_, canvas, flags, params,
                         /*raster_context_provider=*/nullptr);
}

void MbAudioPlayer::OnFirstFrameDecoded(scoped_refptr<media::VideoFrame> frame) {
  // Whether or not a frame came back, advance to HAVE_ENOUGH_DATA so the element is
  // playable; a decoded frame additionally enables drawImage(video) + paint.
  if (frame)
    current_frame_ = std::move(frame);
  ready_state_ = kReadyStateHaveEnoughData;
  client_->ReadyStateChanged();
}

std::optional<media::VideoFrame::ID> MbAudioPlayer::CurrentFrameId() const {
  if (current_frame_)
    return current_frame_->unique_id();
  return std::nullopt;
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
