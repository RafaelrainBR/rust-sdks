#ifndef LIVEKIT_MF_ENCODER_FACTORY_H_
#define LIVEKIT_MF_ENCODER_FACTORY_H_

#include <memory>
#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder_factory.h"

namespace webrtc {

// H264 on the system's hardware Media Foundation encoder (Intel Quick Sync, AMD AMF), with
// OpenH264 taking over if it fails to start or stops mid-stream.
class MfVideoEncoderFactory : public VideoEncoderFactory {
 public:
  static bool IsSupported();

  std::vector<SdpVideoFormat> GetSupportedFormats() const override;
  std::vector<SdpVideoFormat> GetImplementations() const override;
  std::unique_ptr<VideoEncoder> Create(const Environment& env,
                                       const SdpVideoFormat& format) override;
};

}  // namespace webrtc

#endif  // LIVEKIT_MF_ENCODER_FACTORY_H_
