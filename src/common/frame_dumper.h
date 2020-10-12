#pragma once
#include "types.h"
#include <memory>

class FrameDumper
{
public:
  using AudioSample = s16;
  using Timestamp = u64;

  virtual ~FrameDumper() = default;

  ALWAYS_INLINE Timestamp GetTimestampFrequency() const { return m_timestamp_frequency; }
  ALWAYS_INLINE Timestamp GetStartTimestamp() const { return m_start_timestamp; }
  ALWAYS_INLINE u32 GetVideoWidth() const { return m_video_width; }
  ALWAYS_INLINE u32 GetVideoHeight() const { return m_video_height; }
  ALWAYS_INLINE u32 GetAudioChannels() const { return m_audio_channels; }

  virtual bool Open(const char* output_file, u32 output_video_bitrate, u32 output_audio_bitrate, u32 video_width,
                    u32 video_height, float video_fps, u32 audio_sample_rate, u32 audio_channels,
                    Timestamp timestamp_frequency, Timestamp start_timestamp) = 0;
  virtual void Close(Timestamp final_timestamp) = 0;

  virtual void AddVideoFrame(const void* pixels, Timestamp timestamp) = 0;
  virtual void AddAudioFrames(const AudioSample* frames, u32 num_frames, Timestamp timestamp) = 0;

  static std::unique_ptr<FrameDumper> CreateWMFFrameDumper();

protected:
  Timestamp m_timestamp_frequency = 0;
  Timestamp m_start_timestamp = 0;
  u32 m_video_width = 0;
  u32 m_video_height = 0;
  u32 m_audio_channels = 0;
};
