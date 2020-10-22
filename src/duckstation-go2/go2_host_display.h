#pragma once
#include "core/host_display.h"
#include <go2/display.h>
#include <memory>
#include <string>

class Go2HostDisplay final : public HostDisplay
{
public:
  Go2HostDisplay();
  ~Go2HostDisplay();

  RenderAPI GetRenderAPI() const override;
  void* GetRenderDevice() const override;
  void* GetRenderContext() const override;

  bool HasRenderDevice() const override;
  bool HasRenderSurface() const override;

  bool CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device) override;
  bool InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device) override;
  void DestroyRenderDevice() override;

  bool MakeRenderContextCurrent() override;
  bool DoneRenderContextCurrent() override;
  void DestroyRenderSurface() override;

  bool ChangeRenderWindow(const WindowInfo& wi) override;
  void ResizeRenderWindow(s32 new_window_width, s32 new_window_height) override;
  bool CreateResources() override;
  void DestroyResources() override;
  bool SetPostProcessingChain(const std::string_view& config) override;

  std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* data, u32 data_stride,
                                                    bool dynamic = false) override;
  void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                     u32 data_stride) override;
  bool DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height, void* out_data,
                       u32 out_data_stride) override;

  bool SupportsDisplayPixelFormat(HostDisplayPixelFormat format) const override;
  bool BeginSetDisplayPixels(HostDisplayPixelFormat format, u32 width, u32 height, void** out_buffer,
                             u32* out_pitch) override;
  void EndSetDisplayPixels() override;

  void SetVSync(bool enabled) override;

  bool Render() override;

private:
  bool CheckSurface(u32 width, u32 height, HostDisplayPixelFormat format);

  go2_display_t* m_display = nullptr;
  go2_surface_t* m_surface = nullptr;
  go2_presenter_t* m_presenter = nullptr;

  u32 m_surface_width = 0;
  u32 m_surface_height = 0;
  HostDisplayPixelFormat m_surface_format = HostDisplayPixelFormat::Unknown;
};
