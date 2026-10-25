#ifndef LIVEKIT_MF_H264_ENCODER_H_
#define LIVEKIT_MF_H264_ENCODER_H_

#include <winsock2.h>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <strmif.h>
#include <wrl/client.h>

#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "modules/video_coding/codecs/h264/include/h264.h"

namespace webrtc {

// H264 through the system's hardware Media Foundation encoder: Intel Quick Sync, AMD AMF or
// NVIDIA, whichever driver registered one. The same async MFT Chrome uses on Windows.
class MfH264EncoderImpl : public VideoEncoder {
 public:
  // Whether the system has a hardware H264 encoder MFT.
  static bool IsSupported();

  explicit MfH264EncoderImpl(const Environment& env);
  ~MfH264EncoderImpl() override;

  int32_t InitEncode(const VideoCodec* codec_settings,
                     const Settings& settings) override;
  int32_t RegisterEncodeCompleteCallback(
      EncodedImageCallback* callback) override;
  int32_t Release() override;
  int32_t Encode(const VideoFrame& frame,
                 const std::vector<VideoFrameType>* frame_types) override;
  void SetRates(const RateControlParameters& parameters) override;
  EncoderInfo GetEncoderInfo() const override;

 private:
  struct PendingFrame {
    uint32_t rtp_timestamp;
    int64_t ntp_time_ms;
    int64_t render_time_ms;
    VideoRotation rotation;
    std::optional<ColorSpace> color_space;
  };

  HRESULT Configure(uint32_t bitrate_bps, uint32_t fps);
  // Handles queued events; `wait` blocks for the next one. False on a fatal error.
  bool PumpEvents(bool wait);
  bool DeliverOutput();

  const Environment& env_;
  EncodedImageCallback* callback_ = nullptr;
  Microsoft::WRL::ComPtr<IMFActivate> activate_;
  Microsoft::WRL::ComPtr<IMFTransform> encoder_;
  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> events_;
  Microsoft::WRL::ComPtr<ICodecAPI> codec_api_;
  std::string name_ = "Media Foundation H264";
  bool provides_samples_ = true;
  DWORD output_size_ = 0;
  int need_input_ = 0;
  bool sending_ = false;
  bool key_frame_request_ = true;
  int width_ = 0;
  int height_ = 0;
  uint32_t fps_ = 30;
  uint32_t bitrate_bps_ = 0;
  std::deque<PendingFrame> pending_;
  EncodedImage encoded_image_;
  H264BitstreamParser parser_;
  bool com_initialized_ = false;
};

}  // namespace webrtc

#endif  // LIVEKIT_MF_H264_ENCODER_H_
