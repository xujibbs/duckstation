#include "frame_dumper.h"
#include "windows_headers.h"
#include <Mferror.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <vector>
#include <wrl/client.h>

class FrameDumperWMF final : public FrameDumper
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  FrameDumperWMF();
  ~FrameDumperWMF() override;

  bool Open(const char* output_file, u32 output_video_bitrate, u32 output_audio_bitrate, u32 video_width,
            u32 video_height, float video_fps, u32 audio_sample_rate, u32 audio_channels, Timestamp timestamp_frequency,
            Timestamp start_timestamp) override;

  void Close(Timestamp final_timestamp) override;

  void AddVideoFrame(const void* pixels, Timestamp timestamp) override;
  void AddAudioFrames(const AudioSample* frames, u32 num_frames, Timestamp timestamp) override;

private:
  LONGLONG TimestampToMFSampleTime(Timestamp timestamp) const;
  LONGLONG TimestampToMFDuration(Timestamp timestamp) const;
  void WriteLastVideoFrame(Timestamp next_timestamp);
  void WriteLastAudioFrames(Timestamp next_timestamp);

  ComPtr<IMFByteStream> m_byte_stream;
  ComPtr<IMFSinkWriter> m_sink_writer;
  ComPtr<IMFTransform> m_rgb_to_yuv_transform;
  DWORD m_video_stream_index = 0;
  DWORD m_audio_stream_index = 0;

  std::vector<u32> m_last_frame_video;
  std::vector<s16> m_last_frame_audio;
  Timestamp m_last_frame_video_timestamp = 0;
  Timestamp m_last_frame_audio_timestamp = 0;
};