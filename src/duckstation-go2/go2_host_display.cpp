#include "go2_host_display.h"
#include "common/assert.h"
#include "common/log.h"
#include "imgui.h"
#include <array>
#include <drm/drm_fourcc.h>
#include <tuple>
Log_SetChannel(Go2HostDisplay);

Go2HostDisplay::Go2HostDisplay() = default;

Go2HostDisplay::~Go2HostDisplay()
{
  Assert(!m_display && !m_surface && !m_presenter);
}

HostDisplay::RenderAPI Go2HostDisplay::GetRenderAPI() const
{
  return RenderAPI::None;
}

void* Go2HostDisplay::GetRenderDevice() const
{
  return nullptr;
}

void* Go2HostDisplay::GetRenderContext() const
{
  return nullptr;
}

bool Go2HostDisplay::HasRenderDevice() const
{
  return true;
}

bool Go2HostDisplay::HasRenderSurface() const
{
  return true;
}

bool Go2HostDisplay::MakeRenderContextCurrent()
{
  return true;
}

bool Go2HostDisplay::DoneRenderContextCurrent()
{
  return true;
}

void Go2HostDisplay::DestroyRenderSurface()
{
  // noop
}

bool Go2HostDisplay::ChangeRenderWindow(const WindowInfo& wi)
{
  m_window_info = wi;
  return true;
}

bool Go2HostDisplay::CreateResources()
{
  return true;
}

void Go2HostDisplay::DestroyResources()
{
  // noop
}

bool Go2HostDisplay::SetPostProcessingChain(const std::string_view& config)
{
  return false;
}

void Go2HostDisplay::ResizeRenderWindow(s32 new_window_width, s32 new_window_height)
{
  m_window_info.surface_width = static_cast<u32>(new_window_width);
  m_window_info.surface_height = static_cast<u32>(new_window_height);
}

std::unique_ptr<HostDisplayTexture> Go2HostDisplay::CreateTexture(u32 width, u32 height, const void* data,
                                                                  u32 data_stride, bool dynamic /*= false*/)
{
  return nullptr;
}

void Go2HostDisplay::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                                   u32 data_stride)
{
  // noop
}

bool Go2HostDisplay::DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height, void* out_data,
                                     u32 out_data_stride)
{
  return false;
}

void Go2HostDisplay::SetVSync(bool enabled)
{
  // noop
}

bool Go2HostDisplay::CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device)
{
  m_display = go2_display_create();
  if (!m_display)
    return false;

  m_presenter = go2_presenter_create(m_display, DRM_FORMAT_RGB565, 0xff000000);
  if (!m_presenter)
    return false;

  m_window_info = wi;
  m_window_info.surface_width = go2_display_width_get(m_display);
  m_window_info.surface_height = go2_display_height_get(m_display);

  ImGui::GetIO().DisplaySize.x = static_cast<float>(m_window_info.surface_width);
  ImGui::GetIO().DisplaySize.y = static_cast<float>(m_window_info.surface_height);
  unsigned char* pixels;
  int width, height;
  ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  return true;
}

bool Go2HostDisplay::InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device)
{
  return true;
}

void Go2HostDisplay::DestroyRenderDevice()
{
  if (m_surface)
  {
    go2_surface_destroy(m_surface);
    m_surface = nullptr;
  }

  if (m_presenter)
  {
    go2_presenter_destroy(m_presenter);
    m_presenter = nullptr;
  }

  if (m_display)
  {
    go2_display_destroy(m_display);
    m_display = nullptr;
  }
}

static constexpr std::array<int, static_cast<u32>(HostDisplayPixelFormat::Count)> s_display_pixel_format_mapping = {
  {DRM_FORMAT_INVALID, DRM_FORMAT_RGBA8888, DRM_FORMAT_INVALID, DRM_FORMAT_RGB565, DRM_FORMAT_RGBA5551}};

bool Go2HostDisplay::CheckSurface(u32 width, u32 height, HostDisplayPixelFormat format)
{
  if (width <= m_surface_width && height <= m_surface_height && format == m_surface_format)
    return true;

  if (m_surface)
    go2_surface_destroy(m_surface);

  m_surface = go2_surface_create(m_display, width, height, s_display_pixel_format_mapping[static_cast<u32>(format)]);
  if (!m_surface)
    Panic("Failed to create surface");

  m_surface_width = width;
  m_surface_height = height;
  m_surface_format = format;
  return false;
}

bool Go2HostDisplay::SupportsDisplayPixelFormat(HostDisplayPixelFormat format) const
{
  return (s_display_pixel_format_mapping[static_cast<u32>(format)] != DRM_FORMAT_INVALID);
}

bool Go2HostDisplay::BeginSetDisplayPixels(HostDisplayPixelFormat format, u32 width, u32 height, void** out_buffer,
                                           u32* out_pitch)
{
  if (!CheckSurface(width, height, format))
    return false;

  void* map = go2_surface_map(m_surface);
  if (!map)
    return false;

  *out_buffer = map;
  *out_pitch = static_cast<u32>(go2_surface_stride_get(m_surface));
  SetDisplayTexture(m_surface, format, m_surface_width, m_surface_height, 0, 0, width, height);
  return true;
}

void Go2HostDisplay::EndSetDisplayPixels()
{
  go2_surface_unmap(m_surface);
}

bool Go2HostDisplay::Render()
{
  ImGui::Render();

  if (HasDisplayTexture())
  {
    s32 left, top, width, height, left_padding, top_padding;
    CalculateDrawRect(m_window_info.surface_height, m_window_info.surface_width,
                      static_cast<float>(m_window_info.surface_height) /
                        static_cast<float>(m_window_info.surface_width),
                      &left, &top, &width, &height, &left_padding, &top_padding, nullptr, nullptr, true);

    go2_presenter_post(m_presenter, static_cast<go2_surface*>(m_display_texture_handle), m_display_texture_view_x,
                       m_display_texture_view_y, m_display_texture_view_width, m_display_texture_view_height,
                       top + top_padding, left + left_padding, height, width, GO2_ROTATION_DEGREES_270);
  }

  return true;
}
