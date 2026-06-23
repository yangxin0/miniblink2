// mb_sw_frame_sink.cc — minimal cc::LayerTreeFrameSink stub (not the pixel source).
// Status: Phase 1 stub.
#include "miniblink_host/widget/mb_sw_frame_sink.h"

namespace mb {
MbSoftwareFrameSink::MbSoftwareFrameSink() = default;
MbSoftwareFrameSink::~MbSoftwareFrameSink() = default;
// TODO(mb): BindToClient -> accept + report bound; SubmitCompositorFrame -> ack/discard.
// Pixels are read back via paint-record playback in mb_widget.cc, not here.
}  // namespace mb
