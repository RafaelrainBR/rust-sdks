// Defines the Media Foundation and CODECAPI GUIDs used here (selectany, so no duplicates).
#include <initguid.h>

#include "mf_h264_encoder.h"

#include <mferror.h>

#include <algorithm>

#include "api/array_view.h"
#include "common_video/h264/h264_common.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/convert_from.h"

namespace webrtc {

using Microsoft::WRL::ComPtr;

namespace {

bool StartMediaFoundation() {
  static const bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
  return started;
}

// The hardware H264 encoder the system ranks first (MFT_ENUM_FLAG_SORTANDFILTER).
ComPtr<IMFActivate> FindHardwareEncoder(std::string* name) {
  if (!StartMediaFoundation()) {
    return nullptr;
  }
  MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                         MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                         &input, &output, &activates, &count);
  ComPtr<IMFActivate> first;
  if (SUCCEEDED(hr) && count > 0) {
    first = activates[0];
  }
  for (UINT32 i = 0; i < count; i++) {
    activates[i]->Release();
  }
  CoTaskMemFree(activates);
  if (first && name) {
    WCHAR* friendly = nullptr;
    UINT32 length = 0;
    if (SUCCEEDED(first->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
                                            &friendly, &length))) {
      int size = WideCharToMultiByte(CP_UTF8, 0, friendly, -1, nullptr, 0,
                                     nullptr, nullptr);
      if (size > 1) {
        name->assign(size - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, friendly, -1, name->data(), size,
                            nullptr, nullptr);
      }
      CoTaskMemFree(friendly);
    }
  }
  return first;
}

HRESULT SetUInt(ICodecAPI* api, const GUID& key, UINT32 value) {
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_UI4;
  v.ulVal = value;
  return api->SetValue(&key, &v);
}

HRESULT SetBool(ICodecAPI* api, const GUID& key, bool value) {
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_BOOL;
  v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
  return api->SetValue(&key, &v);
}

HRESULT VideoType(GUID subtype, int width, int height, uint32_t fps,
                  ComPtr<IMFMediaType>* type) {
  HRESULT hr = MFCreateMediaType(type->ReleaseAndGetAddressOf());
  if (SUCCEEDED(hr)) hr = (*type)->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (SUCCEEDED(hr)) hr = (*type)->SetGUID(MF_MT_SUBTYPE, subtype);
  if (SUCCEEDED(hr)) hr = MFSetAttributeSize(type->Get(), MF_MT_FRAME_SIZE, width, height);
  if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type->Get(), MF_MT_FRAME_RATE, fps, 1);
  if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type->Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  if (SUCCEEDED(hr)) hr = (*type)->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  return hr;
}

}  // namespace

bool MfH264EncoderImpl::IsSupported() {
  // MFTEnumEx needs COM on whatever thread builds the factory.
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool found = FindHardwareEncoder(nullptr) != nullptr;
  if (SUCCEEDED(co)) {
    CoUninitialize();
  }
  return found;
}

MfH264EncoderImpl::MfH264EncoderImpl(const Environment& env) : env_(env) {}

MfH264EncoderImpl::~MfH264EncoderImpl() {
  Release();
  if (com_initialized_) {
    CoUninitialize();
  }
}

int32_t MfH264EncoderImpl::InitEncode(const VideoCodec* inst,
                                      const Settings& settings) {
  if (!inst || inst->codecType != kVideoCodecH264 || inst->width < 2 ||
      inst->height < 2 || inst->maxFramerate == 0) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  Release();
  if (!com_initialized_) {
    // RPC_E_CHANGED_MODE: COM is already up on this thread, which is enough.
    com_initialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  }
  // NV12 and H264 need even sizes.
  width_ = inst->width & ~1;
  height_ = inst->height & ~1;
  fps_ = inst->maxFramerate;
  bitrate_bps_ = std::max<uint32_t>(inst->startBitrate, 100) * 1000;

  activate_ = FindHardwareEncoder(&name_);
  HRESULT hr = activate_ ? activate_->ActivateObject(IID_PPV_ARGS(&encoder_))
                         : E_NOINTERFACE;
  if (SUCCEEDED(hr)) {
    hr = Configure(bitrate_bps_, fps_);
  }
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "Media Foundation H264 encoder \"" << name_
                      << "\" failed to start (hr=0x" << std::hex << hr
                      << "); using the software encoder";
    Release();
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  encoded_image_._encodedWidth = width_;
  encoded_image_._encodedHeight = height_;
  sending_ = true;
  key_frame_request_ = true;
  RTC_LOG(LS_INFO) << "Media Foundation H264 encoder \"" << name_ << "\": "
                   << width_ << "x" << height_ << " @ " << fps_
                   << " fps, " << bitrate_bps_ << " bps";
  return WEBRTC_VIDEO_CODEC_OK;
}

HRESULT MfH264EncoderImpl::Configure(uint32_t bitrate_bps, uint32_t fps) {
  ComPtr<IMFAttributes> attributes;
  HRESULT hr = encoder_->GetAttributes(&attributes);
  UINT32 async = 0;
  if (SUCCEEDED(hr)) {
    attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async);
  }
  // Hardware encoders are asynchronous; this loop drives only those.
  if (FAILED(hr) || !async) {
    return FAILED(hr) ? hr : E_NOTIMPL;
  }
  hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
  if (SUCCEEDED(hr)) hr = encoder_.As(&events_);
  if (SUCCEEDED(hr)) hr = encoder_.As(&codec_api_);
  if (FAILED(hr)) {
    return hr;
  }
  // Constant bitrate for the congestion controller, no B-frames, frame-by-frame output, and
  // key frames only when WebRTC asks (losses, new viewers).
  SetUInt(codec_api_.Get(), CODECAPI_AVEncCommonRateControlMode,
          eAVEncCommonRateControlMode_CBR);
  SetUInt(codec_api_.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate_bps);
  SetBool(codec_api_.Get(), CODECAPI_AVLowLatencyMode, true);
  SetUInt(codec_api_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
  SetUInt(codec_api_.Get(), CODECAPI_AVEncMPVGOPSize, 0xFFFF);

  // Output first: an encoder lists its inputs only once its output is set. 42e01f is
  // constrained baseline; an encoder without that profile gets plain baseline, which every
  // H264 decoder plays.
  ComPtr<IMFMediaType> output;
  hr = E_FAIL;
  for (UINT32 profile : {static_cast<UINT32>(eAVEncH264VProfile_ConstrainedBase),
                         static_cast<UINT32>(eAVEncH264VProfile_Base)}) {
    hr = VideoType(MFVideoFormat_H264, width_, height_, fps, &output);
    if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AVG_BITRATE, bitrate_bps);
    if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
    if (SUCCEEDED(hr)) hr = encoder_->SetOutputType(0, output.Get(), 0);
    if (SUCCEEDED(hr)) break;
  }
  ComPtr<IMFMediaType> input;
  if (SUCCEEDED(hr)) hr = VideoType(MFVideoFormat_NV12, width_, height_, fps, &input);
  if (SUCCEEDED(hr)) hr = encoder_->SetInputType(0, input.Get(), 0);
  MFT_OUTPUT_STREAM_INFO info{};
  if (SUCCEEDED(hr)) hr = encoder_->GetOutputStreamInfo(0, &info);
  if (FAILED(hr)) {
    return hr;
  }
  provides_samples_ = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                      MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
  output_size_ = info.cbSize ? info.cbSize : width_ * height_ * 3 / 2;
  encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  return encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}

int32_t MfH264EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MfH264EncoderImpl::Release() {
  if (encoder_) {
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }
  // An async encoder holds its event queue and threads until it is shut down.
  if (activate_) {
    activate_->ShutdownObject();
  }
  codec_api_.Reset();
  events_.Reset();
  encoder_.Reset();
  activate_.Reset();
  pending_.clear();
  need_input_ = 0;
  sending_ = false;
  return WEBRTC_VIDEO_CODEC_OK;
}

bool MfH264EncoderImpl::PumpEvents(bool wait) {
  for (;;) {
    ComPtr<IMFMediaEvent> event;
    HRESULT hr = events_->GetEvent(wait ? 0 : MF_EVENT_FLAG_NO_WAIT, &event);
    if (hr == MF_E_NO_EVENTS_AVAILABLE) {
      return true;
    }
    HRESULT status = S_OK;
    MediaEventType type = MEUnknown;
    if (SUCCEEDED(hr)) hr = event->GetStatus(&status);
    if (SUCCEEDED(hr)) hr = event->GetType(&type);
    if (FAILED(hr) || FAILED(status)) {
      RTC_LOG(LS_ERROR) << "Media Foundation H264 encoder event failed (hr=0x"
                        << std::hex << (FAILED(hr) ? hr : status) << ")";
      return false;
    }
    if (type == METransformNeedInput) {
      need_input_++;
    } else if (type == METransformHaveOutput && !DeliverOutput()) {
      return false;
    }
    if (wait) {
      return true;
    }
  }
}

bool MfH264EncoderImpl::DeliverOutput() {
  MFT_OUTPUT_DATA_BUFFER output{};
  output.dwStreamID = 0;
  ComPtr<IMFSample> own;
  if (!provides_samples_) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateSample(&own)) ||
        FAILED(MFCreateMemoryBuffer(output_size_, &buffer)) ||
        FAILED(own->AddBuffer(buffer.Get()))) {
      return false;
    }
    output.pSample = own.Get();
  }
  DWORD status = 0;
  HRESULT hr = encoder_->ProcessOutput(0, 1, &output, &status);
  if (output.pEvents) {
    output.pEvents->Release();
  }
  ComPtr<IMFSample> sample;
  if (provides_samples_) {
    sample.Attach(output.pSample);
  } else {
    sample = own;
  }
  if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
    // The encoder settled on another output type: take it and go on.
    ComPtr<IMFMediaType> type;
    return SUCCEEDED(encoder_->GetOutputAvailableType(0, 0, &type)) &&
           SUCCEEDED(encoder_->SetOutputType(0, type.Get(), 0));
  }
  if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
    return true;
  }
  if (FAILED(hr) || !sample) {
    RTC_LOG(LS_ERROR) << "Media Foundation H264 ProcessOutput failed (hr=0x"
                      << std::hex << hr << ")";
    return false;
  }

  ComPtr<IMFMediaBuffer> buffer;
  BYTE* data = nullptr;
  DWORD length = 0;
  if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) ||
      FAILED(buffer->Lock(&data, nullptr, &length))) {
    return false;
  }
  encoded_image_.SetEncodedData(EncodedImageBuffer::Create(data, length));
  buffer->Unlock();
  encoded_image_.set_size(length);

  // Outputs come back in input order: the frame this one encodes is the oldest pending.
  PendingFrame frame{};
  if (!pending_.empty()) {
    frame = pending_.front();
    pending_.pop_front();
  }
  encoded_image_.SetRtpTimestamp(frame.rtp_timestamp);
  encoded_image_.SetSimulcastIndex(0);
  encoded_image_.ntp_time_ms_ = frame.ntp_time_ms;
  encoded_image_.capture_time_ms_ = frame.render_time_ms;
  encoded_image_.rotation_ = frame.rotation;
  encoded_image_.SetColorSpace(frame.color_space);
  encoded_image_.content_type_ = VideoContentType::UNSPECIFIED;
  encoded_image_.timing_.flags = VideoSendTiming::kInvalid;
  encoded_image_._encodedWidth = width_;
  encoded_image_._encodedHeight = height_;
  encoded_image_._frameType = VideoFrameType::kVideoFrameDelta;
  const uint8_t* bytes = encoded_image_.data();
  for (const H264::NaluIndex& nalu :
       H264::FindNaluIndices(ArrayView<const uint8_t>(bytes, length))) {
    if (H264::ParseNaluType(bytes[nalu.payload_start_offset]) == H264::kIdr) {
      encoded_image_._frameType = VideoFrameType::kVideoFrameKey;
      break;
    }
  }
  parser_.ParseBitstream(encoded_image_);
  encoded_image_.qp_ = parser_.GetLastSliceQp().value_or(-1);

  CodecSpecificInfo info;
  info.codecType = kVideoCodecH264;
  info.codecSpecific.H264.packetization_mode =
      H264PacketizationMode::NonInterleaved;
  const auto result = callback_->OnEncodedImage(encoded_image_, &info);
  if (result.error != EncodedImageCallback::Result::OK) {
    RTC_LOG(LS_WARNING) << "Media Foundation H264: the encoded frame was not taken";
  }
  return true;
}

int32_t MfH264EncoderImpl::Encode(
    const VideoFrame& input_frame,
    const std::vector<VideoFrameType>* frame_types) {
  if (!encoder_ || !callback_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (!sending_) {
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  }
  if (frame_types && !frame_types->empty()) {
    if ((*frame_types)[0] == VideoFrameType::kEmptyFrame) {
      return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
    }
    if ((*frame_types)[0] == VideoFrameType::kVideoFrameKey) {
      key_frame_request_ = true;
    }
  }
  scoped_refptr<I420BufferInterface> i420 =
      input_frame.video_frame_buffer()->ToI420();
  if (!i420 || i420->width() < width_ || i420->height() < height_) {
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }

  // The encoder says when it takes another frame.
  while (need_input_ == 0) {
    if (!PumpEvents(true)) {
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }
  }

  const DWORD size = width_ * height_ * 3 / 2;
  ComPtr<IMFMediaBuffer> buffer;
  ComPtr<IMFSample> sample;
  BYTE* data = nullptr;
  HRESULT hr = MFCreateMemoryBuffer(size, &buffer);
  if (SUCCEEDED(hr)) hr = buffer->Lock(&data, nullptr, nullptr);
  if (SUCCEEDED(hr)) {
    libyuv::I420ToNV12(i420->DataY(), i420->StrideY(), i420->DataU(),
                       i420->StrideU(), i420->DataV(), i420->StrideV(), data,
                       width_, data + width_ * height_, width_, width_,
                       height_);
    buffer->Unlock();
    hr = buffer->SetCurrentLength(size);
  }
  if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
  if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
  if (SUCCEEDED(hr)) hr = sample->SetSampleTime(input_frame.timestamp_us() * 10);
  if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(10'000'000 / std::max<uint32_t>(fps_, 1));
  if (SUCCEEDED(hr) && key_frame_request_) {
    SetUInt(codec_api_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
    key_frame_request_ = false;
  }
  if (SUCCEEDED(hr)) hr = encoder_->ProcessInput(0, sample.Get(), 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "Media Foundation H264 ProcessInput failed (hr=0x"
                      << std::hex << hr << ")";
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  need_input_--;
  pending_.push_back({input_frame.rtp_timestamp(), input_frame.ntp_time_ms(),
                      input_frame.render_time_ms(), input_frame.rotation(),
                      input_frame.color_space()});

  // In low latency mode each frame's output follows it: take it now rather than with the
  // next frame, but never wait past the moment the encoder asks for more input.
  const size_t waiting = pending_.size();
  for (int i = 0; i < 50 && pending_.size() == waiting && need_input_ == 0; i++) {
    if (!PumpEvents(false)) {
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }
    if (pending_.size() == waiting && need_input_ == 0) {
      Sleep(1);
    }
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

void MfH264EncoderImpl::SetRates(const RateControlParameters& parameters) {
  const uint32_t bitrate_bps = parameters.bitrate.get_sum_bps();
  if (bitrate_bps == 0) {
    sending_ = false;
    return;
  }
  if (!sending_) {
    key_frame_request_ = true;
  }
  sending_ = encoder_ != nullptr;
  if (codec_api_ && bitrate_bps != bitrate_bps_) {
    SetUInt(codec_api_.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate_bps);
    bitrate_bps_ = bitrate_bps;
  }
}

VideoEncoder::EncoderInfo MfH264EncoderImpl::GetEncoderInfo() const {
  EncoderInfo info;
  info.implementation_name = name_;
  info.is_hardware_accelerated = true;
  info.supports_native_handle = false;
  info.supports_simulcast = false;
  // OpenH264's QP thresholds: a weak encoder at its limit can drop resolution.
  info.scaling_settings = VideoEncoder::ScalingSettings(24, 37);
  info.preferred_pixel_formats = {VideoFrameBuffer::Type::kI420};
  return info;
}

}  // namespace webrtc
