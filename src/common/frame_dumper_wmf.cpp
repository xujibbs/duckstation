#include "frame_dumper_wmf.h"
#include "log.h"
#include "string_util.h"
#include <atomic>
#include <wmcodecdsp.h>
Log_SetChannel(FrameDumperWMF);

#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "mf")

static std::atomic_uint32_t s_mf_refcount{0};
static bool s_com_initialized_by_us = false;

static bool InitializeMF()
{
  if (s_mf_refcount.fetch_add(1) > 0)
    return true;

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  s_com_initialized_by_us = SUCCEEDED(hr);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE)
  {
    Log_ErrorPrintf("Failed to initialize COM");
    s_mf_refcount.fetch_sub(1);
    return false;
  }

  hr = MFStartup(MF_API_VERSION);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("MFStartup() failed: %08X", hr);
    if (s_com_initialized_by_us)
    {
      s_com_initialized_by_us = false;
      CoUninitialize();
    }

    s_mf_refcount.fetch_sub(1);
    return false;
  }

  return true;
}

static void ShutdownMF()
{
  if (s_mf_refcount.fetch_sub(1) > 1)
    return;

  MFShutdown();

  if (s_com_initialized_by_us)
  {
    CoUninitialize();
    s_com_initialized_by_us = false;
  }
}

static void LogHR(const char* reason, HRESULT hr)
{
  Log_ErrorPrintf("%s failed: %08X", reason, hr);
}

FrameDumperWMF::FrameDumperWMF()
{
  InitializeMF();
}

FrameDumperWMF::~FrameDumperWMF()
{
  if (m_sink_writer)
    Close(std::max(m_last_frame_audio_timestamp, m_last_frame_video_timestamp) + 1);

  ShutdownMF();
}

std::unique_ptr<FrameDumper> FrameDumper::CreateWMFFrameDumper()
{
  return std::make_unique<FrameDumperWMF>();
}

bool FrameDumperWMF::Open(const char* output_file, u32 output_video_bitrate, u32 output_audio_bitrate, u32 video_width,
                          u32 video_height, float video_fps, u32 audio_sample_rate, u32 audio_channels,
                          Timestamp timestamp_frequency, Timestamp start_timestamp)
{
  ComPtr<IMFMediaType> video_out_media_type;
  HRESULT hr = MFCreateMediaType(video_out_media_type.GetAddressOf());
  if (SUCCEEDED(hr))
    hr = video_out_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (SUCCEEDED(hr))
    hr = video_out_media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  if (SUCCEEDED(hr))
    hr = video_out_media_type->SetUINT32(MF_MT_AVG_BITRATE, output_video_bitrate);
  if (SUCCEEDED(hr))
    hr = video_out_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (SUCCEEDED(hr))
    hr = MFSetAttributeSize(video_out_media_type.Get(), MF_MT_FRAME_SIZE, video_width, video_height);
  if (SUCCEEDED(hr))
    hr = MFSetAttributeRatio(video_out_media_type.Get(), MF_MT_FRAME_RATE, 60, 1);
  if (SUCCEEDED(hr))
    hr = MFSetAttributeRatio(video_out_media_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  // if (SUCCEEDED(hr))
  // hr = video_out_media_type->SetBlob(MF_MT_MPEG4_SAMPLE_DESCRIPTION, nullptr, 0); // TODO: Is this needed?
  if (FAILED(hr))
  {
    LogHR("Setting up output video type", hr);
    return false;
  }

  ComPtr<IMFMediaType> video_in_media_type;
  hr = MFCreateMediaType(video_in_media_type.GetAddressOf());
  if (SUCCEEDED(hr))
    hr = video_in_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (SUCCEEDED(hr))
    hr = video_in_media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  if (SUCCEEDED(hr))
    hr = video_in_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (SUCCEEDED(hr))
    hr = MFSetAttributeSize(video_in_media_type.Get(), MF_MT_FRAME_SIZE, video_width, video_height);
  if (SUCCEEDED(hr))
    hr = MFSetAttributeRatio(video_in_media_type.Get(), MF_MT_FRAME_RATE, 60, 1);
  if (SUCCEEDED(hr))
    hr = MFSetAttributeRatio(video_in_media_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  if (FAILED(hr))
  {
    LogHR("Setting up input video type", hr);
    return false;
  }

  ComPtr<IMFMediaType> video_in_yuv_media_type;
  hr = MFCreateMediaType(video_in_yuv_media_type.GetAddressOf());
  if (SUCCEEDED(hr))
    hr = video_in_yuv_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (SUCCEEDED(hr))
    hr = video_in_yuv_media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_YUY2);
  if (SUCCEEDED(hr))
    hr = video_in_yuv_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (SUCCEEDED(hr))
    hr = MFSetAttributeSize(video_in_yuv_media_type.Get(), MF_MT_FRAME_SIZE, video_width, video_height);
  if (SUCCEEDED(hr))
    hr = MFSetAttributeRatio(video_in_yuv_media_type.Get(), MF_MT_FRAME_RATE, 60, 1);
  if (SUCCEEDED(hr))
    hr = MFSetAttributeRatio(video_in_yuv_media_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  if (FAILED(hr))
  {
    LogHR("Setting up input yuv video type", hr);
    return false;
  }

  ComPtr<IMFMediaType> audio_out_media_type;
  hr = MFCreateMediaType(audio_out_media_type.GetAddressOf());
  if (SUCCEEDED(hr))
    hr = audio_out_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  if (SUCCEEDED(hr))
    hr = audio_out_media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
  if (SUCCEEDED(hr))
    hr = audio_out_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, sizeof(AudioSample) * 8);
  if (SUCCEEDED(hr))
    hr = audio_out_media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audio_sample_rate);
  if (SUCCEEDED(hr))
    hr = audio_out_media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audio_channels);
  if (SUCCEEDED(hr))
  {
    hr = audio_out_media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                         (static_cast<u32>(audio_sample_rate) * audio_channels * output_audio_bitrate) /
                                           8);
  }
  if (FAILED(hr))
  {
    LogHR("Setting up output audio type", hr);
    return false;
  }

  ComPtr<IMFMediaType> audio_in_media_type;
  hr = MFCreateMediaType(audio_in_media_type.GetAddressOf());
  if (SUCCEEDED(hr))
    hr = audio_in_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  if (SUCCEEDED(hr))
    hr = audio_in_media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
  if (SUCCEEDED(hr))
    hr = audio_in_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, sizeof(AudioSample) * 8);
  if (SUCCEEDED(hr))
    hr = audio_in_media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audio_sample_rate);
  if (SUCCEEDED(hr))
    hr = audio_in_media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audio_channels);
  if (FAILED(hr))
  {
    LogHR("Setting up output audio type", hr);
    return false;
  }

  hr = CoCreateInstance(__uuidof(CColorConvertDMO), NULL, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(m_rgb_to_yuv_transform.ReleaseAndGetAddressOf()));
  if (FAILED(hr))
  {
    LogHR("CoCreateInstance(CLSID_CColorConvertDMO)", hr);
    return false;
  }

  hr = m_rgb_to_yuv_transform->SetInputType(0, video_in_media_type.Get(), 0);
  if (SUCCEEDED(hr))
    m_rgb_to_yuv_transform->SetOutputType(0, video_in_yuv_media_type.Get(), 0);
  if (FAILED(hr))
  {
    LogHR("Set up YUV transform", hr);
    return false;
  }

  ComPtr<IMFTransform> h264;
  hr = CoCreateInstance(__uuidof(CMSH264EncoderMFT), NULL, CLSCTX_INPROC_SERVER,
    IID_PPV_ARGS(h264.ReleaseAndGetAddressOf()));
  if (FAILED(hr))
  {
    LogHR("blah", hr);
    return false;
  }

  hr = h264->SetOutputType(0, video_out_media_type.Get(), 0); 
  if (SUCCEEDED(hr))
    hr = h264->SetInputType(0, video_in_yuv_media_type.Get(), 0);
    
  if (FAILED(hr))
    return false;

  const std::wstring wfilename(StringUtil::UTF8StringToWideString(output_file));

  hr = MFCreateFile(MF_ACCESSMODE_WRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE, wfilename.c_str(),
                    m_byte_stream.ReleaseAndGetAddressOf());
  if (FAILED(hr))
  {
    LogHR("MFCreateFile", hr);
    return false;
  }

  ComPtr<IMFMediaSink> sink;
  hr = MFCreateMPEG4MediaSink(m_byte_stream.Get(), video_out_media_type.Get(), audio_out_media_type.Get(),
                              sink.GetAddressOf());
  if (FAILED(hr))
  {
    LogHR("MFCreateMPEG4MediaSink", hr);
    return false;
  }

  hr = MFCreateSinkWriterFromMediaSink(sink.Get(), nullptr, m_sink_writer.ReleaseAndGetAddressOf());
  if (FAILED(hr))
  {
    LogHR("MFCreateSinkWriterFromURL", hr);
    return false;
  }

  m_video_stream_index = 0;
  m_audio_stream_index = 1;

  //   hr = m_sink_writer->AddStream(video_out_media_type.Get(), &m_video_stream_index);
  //   if (FAILED(hr))
  //   {
  //     LogHR("AddStream(Video)", hr);
  //     return false;
  //   }

  hr = m_sink_writer->SetInputMediaType(m_video_stream_index, video_in_yuv_media_type.Get(), nullptr);
  if (FAILED(hr))
  {
    LogHR("SetInputMediaType(Video)", hr);
    return false;
  }

  //   hr = m_sink_writer->AddStream(audio_out_media_type.Get(), &m_audio_stream_index);
  //   if (FAILED(hr))
  //   {
  //     LogHR("AddStream(Audio)", hr);
  //     return false;
  //   }

  hr = m_sink_writer->SetInputMediaType(m_audio_stream_index, audio_in_media_type.Get(), nullptr);
  if (FAILED(hr))
  {
    LogHR("SetInputMediaType(Audio)", hr);
    return false;
  }

  hr = m_sink_writer->BeginWriting();
  if (FAILED(hr))
  {
    LogHR("BeginWriting", hr);
    return false;
  }

  m_timestamp_frequency = timestamp_frequency;
  m_start_timestamp = start_timestamp;
  m_video_width = video_width;
  m_video_height = video_height;
  m_audio_channels = audio_channels;
  return true;
}

void FrameDumperWMF::Close(Timestamp final_timestamp)
{
  WriteLastVideoFrame(final_timestamp);
  WriteLastAudioFrames(final_timestamp);

  HRESULT hr = m_sink_writer->Finalize();
  if (FAILED(hr))
    LogHR("Finalize", hr);

  m_sink_writer.Reset();
}

void FrameDumperWMF::AddVideoFrame(const void* pixels, Timestamp timestamp)
{
  WriteLastVideoFrame(timestamp);

  m_last_frame_video_timestamp = timestamp;
  m_last_frame_video.resize(m_video_width * m_video_height);
  std::memcpy(m_last_frame_video.data(), pixels, m_video_width * m_video_height * sizeof(u32));
}

void FrameDumperWMF::AddAudioFrames(const AudioSample* frames, u32 num_frames, Timestamp timestamp)
{
  if (timestamp == m_last_frame_audio_timestamp)
  {
    const u32 start = static_cast<u32>(m_last_frame_audio.size());
    m_last_frame_audio.resize(start + (num_frames * m_audio_channels));
    std::memcpy(m_last_frame_audio.data() + start, frames, num_frames * m_audio_channels * sizeof(AudioSample));
  }
  else
  {
    WriteLastAudioFrames(timestamp);

    m_last_frame_audio_timestamp = timestamp;
    m_last_frame_audio.resize(num_frames * m_audio_channels);
    std::memcpy(m_last_frame_audio.data(), frames, num_frames * m_audio_channels * sizeof(AudioSample));
  }
}

LONGLONG FrameDumperWMF::TimestampToMFSampleTime(Timestamp timestamp) const
{
  // return in 100-nanosecond units
  const Timestamp ticks_since_start = timestamp - m_start_timestamp;
  return (ticks_since_start * 10000000) / m_timestamp_frequency;
}

LONGLONG FrameDumperWMF::TimestampToMFDuration(Timestamp timestamp) const
{
  // return in 100-nanosecond units
  const Timestamp num = (timestamp * 10000000);
  return (num /*+ (m_timestamp_frequency - 1)*/) / m_timestamp_frequency;
}

static Microsoft::WRL::ComPtr<IMFMediaBuffer> AllocAndCopy(u32 data_size, const void* data)
{
  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = MFCreateMemoryBuffer(data_size, buffer.GetAddressOf());
  if (FAILED(hr))
  {
    LogHR("MFCreateMemoryBuffer", hr);
    return {};
  }

  BYTE* mapped_ptr = nullptr;
  hr = buffer->SetCurrentLength(data_size);
  if (SUCCEEDED(hr))
    hr = buffer->Lock(&mapped_ptr, nullptr, nullptr);
  if (SUCCEEDED(hr))
  {
    std::memcpy(mapped_ptr, data, data_size);
    hr = buffer->Unlock();
  }
  if (FAILED(hr))
  {
    LogHR("Buffer lock and upload", hr);
    return {};
  }

  return buffer;
}

static Microsoft::WRL::ComPtr<IMFSample> AllocAndCopySample(u32 data_size, const void* data, LONGLONG start_time,
                                                            LONGLONG duration)
{
  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer = AllocAndCopy(data_size, data);
  if (!buffer)
    return {};

  Microsoft::WRL::ComPtr<IMFSample> sample;
  HRESULT hr = MFCreateSample(sample.GetAddressOf());
  if (FAILED(hr))
  {
    LogHR("MFCreateSample", hr);
    return {};
  }

  hr = sample->AddBuffer(buffer.Get());
  if (FAILED(hr))
  {
    LogHR("AddBuffer", hr);
    return {};
  }

  hr = sample->SetSampleTime(start_time);
  if (SUCCEEDED(hr))
    hr = sample->SetSampleDuration(duration);
  if (FAILED(hr))
  {
    LogHR("SetSample{Time,Duration}", hr);
    return {};
  }

  return sample;
}

void FrameDumperWMF::WriteLastVideoFrame(Timestamp next_timestamp)
{
  if (m_last_frame_video.empty())
    return;

  const u32 data_size = m_video_width * m_video_height * sizeof(u32);
  const LONGLONG start_time = TimestampToMFSampleTime(m_last_frame_video_timestamp);
  const LONGLONG duration = TimestampToMFDuration(next_timestamp - m_last_frame_video_timestamp);
  Log_InfoPrintf("Write video frame @ %llu for %llu", start_time, duration);
  ComPtr<IMFSample> sample = AllocAndCopySample(data_size, m_last_frame_video.data(), start_time, duration);
  if (sample)
  {
    HRESULT hr = m_rgb_to_yuv_transform->ProcessInput(0, sample.Get(), 0);
    if (SUCCEEDED(hr))
    {
      MFT_OUTPUT_DATA_BUFFER dbuf = {};
      DWORD status = 0;
      hr = m_rgb_to_yuv_transform->ProcessOutput(0, 1, &dbuf, &status);
      if (SUCCEEDED(hr))
      {
        HRESULT hr = m_sink_writer->WriteSample(m_video_stream_index, dbuf.pSample);
        if (FAILED(hr))
          LogHR("WriteSample(Video)", hr);

        dbuf.pSample->Release();
      }
      else
      {
        LogHR("ProcessOutput", hr);
      }
    }
  }

  m_last_frame_video.clear();
  m_last_frame_video_timestamp = 0;
}

void FrameDumperWMF::WriteLastAudioFrames(Timestamp next_timestamp)
{
  if (m_last_frame_audio.empty())
    return;

#if 1
  const u32 data_size = static_cast<u32>(m_last_frame_audio.size()) * sizeof(AudioSample);
  const LONGLONG start_time = TimestampToMFSampleTime(m_last_frame_audio_timestamp);
  const LONGLONG duration = TimestampToMFDuration(next_timestamp - m_last_frame_audio_timestamp);
  Log_InfoPrintf("Write %u audio frames @ %llu for %llu", u32(m_last_frame_audio.size()) / m_audio_channels, start_time,
                 duration);
  ComPtr<IMFSample> sample = AllocAndCopySample(data_size, m_last_frame_audio.data(), start_time, duration);
  if (sample)
  {
    HRESULT hr = m_sink_writer->WriteSample(m_audio_stream_index, sample.Get());
    if (FAILED(hr))
      LogHR("WriteSample(Audio)", hr);
  }
#endif

  m_last_frame_audio.clear();
  m_last_frame_audio_timestamp = 0;
}
