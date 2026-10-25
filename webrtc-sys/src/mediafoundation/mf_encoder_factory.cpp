#include "mf_encoder_factory.h"

#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#include "api/video_codecs/video_encoder_software_fallback_wrapper.h"
#include "mf_h264_encoder.h"

namespace webrtc {

bool MfVideoEncoderFactory::IsSupported() {
  static const bool supported = MfH264EncoderImpl::IsSupported();
  return supported;
}

// The one H264 profile the app sends first (desktop-native share::preferred_video_codecs), as
// the NVENC factory advertises it.
std::vector<SdpVideoFormat> MfVideoEncoderFactory::GetSupportedFormats() const {
  return {SdpVideoFormat("H264", {{"profile-level-id", "42e01f"},
                                  {"level-asymmetry-allowed", "1"},
                                  {"packetization-mode", "1"}})};
}

std::vector<SdpVideoFormat> MfVideoEncoderFactory::GetImplementations() const {
  return GetSupportedFormats();
}

std::unique_ptr<VideoEncoder> MfVideoEncoderFactory::Create(
    const Environment& env,
    const SdpVideoFormat& format) {
  auto hardware = std::make_unique<MfH264EncoderImpl>(env);
  auto software = OpenH264EncoderTemplateAdapter::CreateEncoder(env, format);
  if (!software) {
    return hardware;
  }
  return CreateVideoEncoderSoftwareFallbackWrapper(env, std::move(software),
                                                   std::move(hardware), false);
}

}  // namespace webrtc
