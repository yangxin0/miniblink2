// mb_media_player.cc — construct a real blink::WebMediaPlayerImpl. See the header.
//
// Mirrors a MINIMAL subset of content/renderer/media/media_factory.cc's CreateMediaPlayer:
// a RendererFactorySelector over the standard RendererImplFactory (local audio+video
// renderer with A/V sync) fed by media::DefaultDecoderFactory (software VP9/AV1/Opus/
// FFmpeg), constructed via blink::WebMediaPlayerBuilder (which owns the UrlIndex). The
// software/no-GPU choices: use_surface_layer_for_video=false (no viz SurfaceLayer; our
// main frame paints in software), null raster context, null video-frame submitter.

#include "miniblink_host/media/mb_media_player.h"

#include <tuple>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/thread.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/base/audio_renderer_sink.h"
#include "media/base/fake_audio_worker.h"
#include "media/base/output_device_info.h"
#include "media/base/media_log.h"
#include "media/base/media_player_logging_id.h"
#include "media/base/media_util.h"  // media::NullMediaLog
#include "media/base/renderer.h"
#include "media/base/renderer_factory_selector.h"
#include "media/mojo/mojom/media_metrics_provider.mojom.h"
#include "media/renderers/default_decoder_factory.h"
#include "media/renderers/renderer_impl_factory.h"
#include "media/video/gpu_video_accelerator_factories.h"  // complete type for GetGpuFactoriesCB
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "components/viz/common/gpu/raster_context_provider.h"  // complete (null scoped_refptr arg)
#include "third_party/blink/public/common/thread_safe_browser_interface_broker_proxy.h"  // WrapRefCounted
#include "third_party/blink/public/platform/media/web_media_player_builder.h"
#include "third_party/blink/public/platform/media/web_media_player_delegate.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/web_video_frame_submitter.h"  // null submitter arg
#include "third_party/blink/public/web/web_local_frame.h"

namespace mb {
namespace {

// A silent SwitchableAudioRendererSink that still drives the playback CLOCK: it pulls the
// render callback at real time via a FakeAudioWorker (so AudioRendererImpl's audio clock
// advances and the video stays in sync) and discards the samples. Unlike
// media::NullAudioSink it has NO SequenceChecker, so it tolerates being created on the main
// thread (in MbCreateWebMediaPlayer) and used on the media thread, where WMPI's pipeline
// runs. Step 2 replaces this with a real CoreAudio output sink for actual sound.
class MbSilentAudioSink : public media::SwitchableAudioRendererSink {
 public:
  explicit MbSilentAudioSink(scoped_refptr<base::SequencedTaskRunner> runner)
      : task_runner_(std::move(runner)) {}

  void Initialize(const media::AudioParameters& params,
                  RenderCallback* callback) override {
    // Runs on the media thread (AudioRendererImpl::Initialize); the worker too.
    fake_worker_ = std::make_unique<media::FakeAudioWorker>(task_runner_, params);
    fixed_delay_ = media::FakeAudioWorker::ComputeFakeOutputDelay(params);
    audio_bus_ = media::AudioBus::Create(params);
    callback_ = callback;
  }
  void Start() override {}
  void Stop() override {
    if (fake_worker_)
      fake_worker_->Stop();
  }
  void Play() override {
    if (playing_ || !fake_worker_)
      return;
    playing_ = true;
    fake_worker_->Start(base::BindRepeating(&MbSilentAudioSink::CallRender,
                                            base::Unretained(this)));
  }
  void Pause() override {
    if (playing_ && fake_worker_) {
      fake_worker_->Stop();
      playing_ = false;
    }
  }
  void Flush() override {}
  bool SetVolume(double) override { return true; }  // always muted
  media::OutputDeviceInfo GetOutputDeviceInfo() override {
    return media::OutputDeviceInfo(media::OUTPUT_DEVICE_STATUS_OK);
  }
  void GetOutputDeviceInfoAsync(OutputDeviceInfoCB cb) override {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(cb), GetOutputDeviceInfo()));
  }
  bool IsOptimizedForHardwareParameters() override { return false; }
  bool CurrentThreadIsRenderingThread() override {
    return task_runner_->RunsTasksInCurrentSequence();
  }
  void SwitchOutputDevice(const std::string&,
                          media::OutputDeviceStatusCB cb) override {
    std::move(cb).Run(media::OUTPUT_DEVICE_STATUS_ERROR_INTERNAL);
  }

 protected:
  ~MbSilentAudioSink() override = default;

 private:
  void CallRender(base::TimeTicks ideal_time, base::TimeTicks /*now*/) {
    // Idealized delay -> smoothest A/V sync (see media::AudioClock); samples discarded.
    callback_->Render(fixed_delay_, ideal_time, {}, audio_bus_.get());
  }

  scoped_refptr<base::SequencedTaskRunner> task_runner_;
  std::unique_ptr<media::FakeAudioWorker> fake_worker_;
  std::unique_ptr<media::AudioBus> audio_bus_;
  base::TimeDelta fixed_delay_;
  RenderCallback* callback_ = nullptr;
  bool playing_ = false;
};

// Minimal WebMediaPlayerDelegate: we don't track page/frame visibility or media-session
// state (single view, always "visible"). AddObserver hands back a unique id WMPI keys its
// later calls on; everything else is a no-op / negative.
class MbMediaDelegate : public blink::WebMediaPlayerDelegate {
 public:
  bool IsPageHidden() override { return false; }
  bool IsFrameHidden() override { return false; }
  int AddObserver(Observer*) override { return ++next_id_; }
  void RemoveObserver(int) override {}
  void DidMediaMetadataChange(int,
                              bool,
                              bool,
                              media::MediaContentType) override {}
  void DidPlay(int) override {}
  void DidPause(int, bool) override {}
  void PlayerGone(int) override {}
  void SetIdle(int, bool) override {}
  bool IsIdle(int) override { return false; }
  void ClearStaleFlag(int) override {}
  bool IsStale(int) override { return false; }

 private:
  int next_id_ = 0;
};

// Process-wide media infrastructure shared by every player: the media + video-frame-
// compositor threads, the software decoder factory, and the delegate. Leaked (process
// lifetime), matching the engine's other globals.
class MbMediaSupport {
 public:
  static MbMediaSupport& Get() {
    static base::NoDestructor<MbMediaSupport> s;
    return *s;
  }

  media::DecoderFactory* decoder_factory() { return decoder_factory_.get(); }
  blink::WebMediaPlayerDelegate* delegate() { return &delegate_; }
  scoped_refptr<base::SequencedTaskRunner> media_task_runner() {
    return media_thread_.task_runner();
  }
  // The VideoFrameCompositor MUST run off the main thread (WMPI::Paint reads the current
  // frame from it); a dedicated thread avoids a same-thread post/wait.
  scoped_refptr<base::SingleThreadTaskRunner> vfc_task_runner() {
    return vfc_thread_.task_runner();
  }

 private:
  friend class base::NoDestructor<MbMediaSupport>;
  MbMediaSupport() : media_thread_("mb-media"), vfc_thread_("mb-media-vfc") {
    media_thread_.Start();
    vfc_thread_.Start();
    // No external (GPU/mojo) decoder factory -> DefaultDecoderFactory uses the built-in
    // software decoders (OffloadingVpxVideoDecoder=VP9, Dav1d=AV1, Opus, FFmpeg).
    decoder_factory_ = std::make_unique<media::DefaultDecoderFactory>(nullptr);
  }

  base::Thread media_thread_;
  base::Thread vfc_thread_;
  std::unique_ptr<media::DefaultDecoderFactory> decoder_factory_;
  MbMediaDelegate delegate_;
};

}  // namespace

std::unique_ptr<blink::WebMediaPlayer> MbCreateWebMediaPlayer(
    blink::WebMediaPlayerBuilder& builder,
    blink::WebLocalFrame* frame,
    blink::WebMediaPlayerClient* client) {
  if (!frame || !client)
    return nullptr;
  MbMediaSupport& support = MbMediaSupport::Get();

  auto media_log = std::make_unique<media::NullMediaLog>();
  const media::MediaPlayerLoggingID player_id =
      media::GetNextMediaPlayerLoggingID();

  // The local audio+video renderer (RendererImpl) with A/V sync, fed by the software
  // decoders. No GPU video-accelerator factories (software-only decode).
  auto factory_selector = std::make_unique<media::RendererFactorySelector>();
  factory_selector->AddBaseFactory(
      media::RendererType::kRendererImpl,
      std::make_unique<media::RendererImplFactory>(
          media_log.get(), support.decoder_factory(),
          base::BindRepeating(
              []() -> media::GpuVideoAcceleratorFactories* { return nullptr; }),
          player_id,
          // Non-Android RendererImplFactory takes a SpeechRecognitionClient (for live
          // captions); we don't support that, so pass none.
          /*speech_recognition_client=*/nullptr));

  // Silent sink that still drives the playback clock (so video advances at real speed).
  // Step 2 replaces this with a real CoreAudio output sink for sound.
  auto audio_sink =
      base::MakeRefCounted<MbSilentAudioSink>(support.media_task_runner());

  // MbEmptyBroker has no MediaMetricsProvider, so leave the remote disconnected (its
  // receiver end is dropped) — WMPI binds it and metric calls are silently discarded.
  mojo::PendingRemote<media::mojom::MediaMetricsProvider> metrics_provider;
  std::ignore = metrics_provider.InitWithNewPipeAndPassReceiver();

  auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();

  // NOTE: `builder` is owned by the caller (MbFrameClient) and outlives this player —
  // it holds the UrlIndex the player dereferences on a later async DoLoad.
  return builder.Build(
      frame, client, /*encrypted_client=*/nullptr, support.delegate(),
      std::move(factory_selector), /*video_frame_submitter=*/nullptr,
      std::move(media_log), player_id,
      // DeferLoadCB: never defer — run the load closure immediately.
      base::BindRepeating([](base::OnceClosure cb) {
        std::move(cb).Run();
        return false;
      }),
      std::move(audio_sink),
      /*media_task_runner=*/support.media_task_runner(),
      /*worker_task_runner=*/
      base::ThreadPool::CreateTaskRunner({base::MayBlock()}),
      /*compositor_task_runner=*/runner,
      /*video_frame_compositor_task_runner=*/support.vfc_task_runner(),
      /*initial_cdm=*/nullptr,
      /*request_routing_token_cb=*/media::RequestRoutingTokenCallback(),
      /*media_observer=*/base::WeakPtr<media::MediaObserver>(),
      /*embedded_media_experience_enabled=*/false, std::move(metrics_provider),
      // create_bridge_callback: unused (use_surface_layer_for_video=false).
      blink::CreateSurfaceLayerBridgeCB(),
      /*raster_context_provider=*/nullptr,
      /*use_surface_layer_for_video=*/false,
      /*is_background_suspend_enabled=*/false,
      /*is_background_video_playback_enabled=*/true,
      /*is_background_video_track_optimization_supported=*/false,
      /*demuxer_override=*/nullptr,
      // Build wants a scoped_refptr; Platform returns a raw (refcounted) pointer.
      base::WrapRefCounted(
          blink::Platform::Current()->GetBrowserInterfaceBroker()));
}

}  // namespace mb
