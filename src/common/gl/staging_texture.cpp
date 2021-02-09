#include "staging_texture.h"
#include "../align.h"
#include "../assert.h"
#include "../log.h"
#include "texture.h"
Log_SetChannel(GL);

namespace GL {

static GLenum GetGLFormat(GLenum format)
{
  switch (format)
  {
    case GL_RGBA8:
      return GL_RGBA;

    default:
      Panic("Bad format");
      return GL_UNSIGNED_BYTE;
  }
}

static GLenum GetGLType(GLenum format)
{
  switch (format)
  {
    case GL_RGBA8:
      return GL_UNSIGNED_BYTE;

    default:
      Panic("Bad format");
      return GL_UNSIGNED_BYTE;
  }
}

static u32 GetPixelSize(GLenum format)
{
  switch (format)
  {
    case GL_RGBA8:
      return sizeof(u32);

    default:
      Panic("Bad format");
      return 4;
  }
}

static bool IsDepthFormat(GLenum format)
{
  return false;
}

static u32 GetStride(GLenum format, u32 width)
{
  return Common::AlignUpPow2(GetPixelSize(format) * width, 4);
}

static bool UsePersistentStagingBuffers()
{
  // We require ARB_buffer_storage to create the persistent mapped buffer,
  // ARB_shader_image_load_store for glMemoryBarrier, and ARB_sync to ensure
  // the GPU has finished the copy before reading the buffer from the CPU.
  const bool has_buffer_storage = (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage || GLAD_GL_EXT_buffer_storage);
  const bool has_shader_image_load_storage =
    (GLAD_GL_VERSION_4_2 || GLAD_GL_ES_VERSION_3_1 || GLAD_GL_ARB_shader_image_load_store);
  const bool has_sync = (GLAD_GL_VERSION_3_2 || GLAD_GL_ES_VERSION_3_0 || GLAD_GL_ARB_sync);
  return has_buffer_storage && has_shader_image_load_storage && has_sync;
}

StagingTexture::StagingTexture() = default;

StagingTexture::~StagingTexture()
{
  Destroy();
}

void StagingTexture::Destroy()
{
  m_width = 0;
  m_height = 0;
  m_format = GL_RGBA8;
  m_stride = 0;
  m_texel_size = 0;

  if (m_fence != 0)
  {
    glDeleteSync(m_fence);
    m_fence = 0;
  }
  if (m_map_pointer)
  {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_buffer_name);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    m_map_pointer = nullptr;
  }
  if (m_buffer_name != 0)
  {
    glDeleteBuffers(1, &m_buffer_name);
    m_buffer_name = 0;
    m_buffer_size = 0;
  }
}

bool StagingTexture::Create(u32 width, u32 height, GLenum format, bool readback)
{
  if (IsValid())
    Destroy();

  m_width = width;
  m_height = height;
  m_texel_size = GetPixelSize(format);
  m_stride = GetStride(format, width);
  m_buffer_size = m_stride * height;
  m_readback = readback;

  const GLenum target = GetTarget();
  glGenBuffers(1, &m_buffer_name);
  glBindBuffer(target, m_buffer_name);

  // Prefer using buffer_storage where possible. This allows us to skip the map/unmap steps.
  if (UsePersistentStagingBuffers())
  {
    GLenum buffer_flags;
    GLenum map_flags;
    if (readback)
    {
      buffer_flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT;
      map_flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT;
    }
    else
    {
      buffer_flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT;
      map_flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_FLUSH_EXPLICIT_BIT;
    }

    if (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage)
      glBufferStorage(target, m_buffer_size, nullptr, buffer_flags);
    else if (GLAD_GL_EXT_buffer_storage)
      glBufferStorageEXT(target, m_buffer_size, nullptr, buffer_flags);
    else
      UnreachableCode();

    m_map_pointer = reinterpret_cast<char*>(glMapBufferRange(target, 0, m_buffer_size, map_flags));
    Assert(m_map_pointer != nullptr);
  }
  else
  {
    // Otherwise, fallback to mapping the buffer each time.
    glBufferData(target, m_buffer_size, nullptr, readback ? GL_STREAM_READ : GL_STREAM_DRAW);
  }
  glBindBuffer(target, 0);

  return true;
}

void StagingTexture::CopyFromTexture(GL::Texture& src_texture, u32 src_x, u32 src_y, u32 src_layer, u32 src_level,
                                     u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  Assert(m_readback);
  Assert((dst_x + width) <= m_width && (dst_y + height) <= m_height);
  Assert((src_x + width) <= src_texture.GetWidth() && (src_y + height) <= src_texture.GetHeight());

  // Unmap the buffer before writing when not using persistent mappings.
  if (!UsePersistentStagingBuffers())
    Unmap();

  // Copy from the texture object to the staging buffer.
  glBindBuffer(GL_PIXEL_PACK_BUFFER, m_buffer_name);
  glPixelStorei(GL_PACK_ROW_LENGTH, m_width);

  const u32 dst_offset = dst_y * m_stride + dst_x * m_texel_size;

  // Prefer glGetTextureSubImage(), when available.
  if (GLAD_GL_VERSION_4_5 || GLAD_GL_ARB_get_texture_sub_image)
  {
    glGetTextureSubImage(src_texture.GetGLId(), src_level, src_x, src_y, src_layer, width, height, 1,
                         GetGLFormat(m_format), GetGLType(m_format), static_cast<GLsizei>(m_buffer_size - dst_offset),
                         reinterpret_cast<void*>(static_cast<uintptr_t>(dst_offset)));
  }
  else
  {
    // Mutate the shared framebuffer.
    src_texture.BindFramebuffer(GL_READ_FRAMEBUFFER);
    if (IsDepthFormat(m_format))
    {
      glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
      glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, src_texture.GetGLId(), src_level, src_layer);
    }
    else
    {
      glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, src_texture.GetGLId(), src_level, src_layer);
      glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, 0, 0, 0);
    }
    glReadPixels(src_x, src_y, width, height, GetGLFormat(m_format), GetGLType(m_format),
                 reinterpret_cast<void*>(static_cast<uintptr_t>(dst_offset)));
  }

  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

  // If we support buffer storage, create a fence for synchronization.
  if (UsePersistentStagingBuffers())
  {
    if (m_fence != 0)
      glDeleteSync(m_fence);

    glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
    m_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  }

  m_needs_flush = true;
}

void StagingTexture::CopyToTexture(u32 src_x, u32 src_y, GL::Texture& dst_texture, u32 dst_x, u32 dst_y, u32 dst_layer,
                                   u32 dst_level, u32 width, u32 height)
{
  Assert(!m_readback);
  Assert((dst_x + width) <= dst_texture.GetWidth() && (dst_y + height) <= dst_texture.GetHeight());
  Assert((src_x + width) <= m_width && (src_y + height) <= m_height);

  const u32 src_offset = src_y * m_stride + src_x * m_texel_size;
  const u32 copy_size = height * m_stride;

  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_buffer_name);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, m_width);

  if (!UsePersistentStagingBuffers())
  {
    // Unmap the buffer before writing when not using persistent mappings.
    if (m_map_pointer)
    {
      glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      m_map_pointer = nullptr;
    }
  }
  else
  {
    // Since we're not using coherent mapping, we must flush the range explicitly.
    if (!m_readback)
      glFlushMappedBufferRange(GL_PIXEL_UNPACK_BUFFER, src_offset, copy_size);
    glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
  }

  // Copy from the staging buffer to the texture object.
  dst_texture.Bind();
  glTexSubImage3D(dst_texture.GetGLTarget(), 0, dst_x, dst_y, dst_layer, width, height, 1, GetGLFormat(m_format),
                  GetGLType(m_format), reinterpret_cast<void*>(static_cast<uintptr_t>(src_offset)));

  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  // If we support buffer storage, create a fence for synchronization.
  if (UsePersistentStagingBuffers())
  {
    if (m_fence != 0)
      glDeleteSync(m_fence);

    m_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  }

  m_needs_flush = true;
}

void StagingTexture::Flush()
{
  // No-op when not using buffer storage, as the transfers happen on Map().
  // m_fence will always be zero in this case.
  if (m_fence == 0)
  {
    m_needs_flush = false;
    return;
  }

  glClientWaitSync(m_fence, 0, GL_TIMEOUT_IGNORED);
  glDeleteSync(m_fence);
  m_fence = 0;
  m_needs_flush = false;
}

bool StagingTexture::Map()
{
  if (m_map_pointer)
    return true;

  // Slow path, map the texture, unmap it later.
  GLenum flags;
  if (m_readback)
    flags = GL_MAP_READ_BIT;
  else
    flags = GL_MAP_WRITE_BIT;

  const GLenum target = GetTarget();
  glBindBuffer(target, m_buffer_name);
  m_map_pointer = reinterpret_cast<char*>(glMapBufferRange(target, 0, m_buffer_size, flags));
  glBindBuffer(target, 0);
  return m_map_pointer != nullptr;
}

void StagingTexture::Unmap()
{
  // No-op with persistent mapped buffers.
  if (!m_map_pointer || UsePersistentStagingBuffers())
    return;

  const GLenum target = GetTarget();
  glBindBuffer(target, m_buffer_name);
  glUnmapBuffer(target);
  glBindBuffer(target, 0);
  m_map_pointer = nullptr;
}

void StagingTexture::ReadTexels(u32 src_x, u32 src_y, u32 width, u32 height, void* out_ptr, u32 out_stride)
{
  Assert(m_readback);
  Assert((src_x + width) <= m_width && (src_y + height) <= m_height);
  PrepareForAccess();

  // Offset pointer to point to start of region being copied out.
  const char* current_ptr = m_map_pointer;
  current_ptr += src_y * m_stride;
  current_ptr += src_x * m_texel_size;

  // Optimal path: same dimensions, same stride.
  if (src_x == 0 && width == m_width && m_stride == out_stride)
  {
    std::memcpy(out_ptr, current_ptr, m_stride * height);
    return;
  }

  size_t copy_size = std::min<u32>(width * m_texel_size, m_stride);
  char* dst_ptr = reinterpret_cast<char*>(out_ptr);
  for (u32 row = 0; row < height; row++)
  {
    std::memcpy(dst_ptr, current_ptr, copy_size);
    current_ptr += m_stride;
    dst_ptr += out_stride;
  }
}

void StagingTexture::ReadTexel(u32 x, u32 y, void* out_ptr)
{
  Assert(m_readback);
  Assert(x < m_width && y < m_height);
  PrepareForAccess();

  const char* src_ptr = m_map_pointer + y * m_stride + x * m_texel_size;
  std::memcpy(out_ptr, src_ptr, m_texel_size);
}

void StagingTexture::WriteTexels(u32 dst_x, u32 dst_y, u32 width, u32 height, const void* in_ptr, u32 in_stride)
{
  Assert(!m_readback);
  Assert((dst_x + width) <= m_width && (dst_y + height) <= m_height);
  PrepareForAccess();

  // Offset pointer to point to start of region being copied to.
  char* current_ptr = m_map_pointer;
  current_ptr += dst_y * m_stride;
  current_ptr += dst_x * m_texel_size;

  // Optimal path: same dimensions, same stride.
  if (dst_x == 0 && width == m_width && m_stride == in_stride)
  {
    std::memcpy(current_ptr, in_ptr, m_stride * height);
    return;
  }

  size_t copy_size = std::min<u32>(width * m_texel_size, m_stride);
  const char* src_ptr = reinterpret_cast<const char*>(in_ptr);
  for (u32 row = 0; row < height; row++)
  {
    std::memcpy(current_ptr, src_ptr, copy_size);
    current_ptr += m_stride;
    src_ptr += in_stride;
  }
}

void StagingTexture::WriteTexel(u32 x, u32 y, const void* in_ptr)
{
  Assert(x < m_width && y < m_height);
  PrepareForAccess();

  char* dest_ptr = m_map_pointer + y * m_stride + x * m_texel_size;
  std::memcpy(dest_ptr, in_ptr, m_texel_size);
}

void StagingTexture::PrepareForAccess()
{
  if (!IsMapped())
  {
    if (!Map())
      Panic("Failed to map staging texture");
  }

  if (m_needs_flush)
    Flush();
}

} // namespace GL
