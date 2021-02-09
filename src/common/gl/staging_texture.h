#pragma once
#include "../types.h"
#include <glad.h>

namespace GL {

class Texture;

class StagingTexture
{
public:
  StagingTexture();
  ~StagingTexture();

  ALWAYS_INLINE bool IsValid() const { return m_buffer_name != 0; }
  ALWAYS_INLINE bool IsMapped() const { return (m_map_pointer != nullptr); }

  bool Create(u32 width, u32 height, GLenum format, bool readback);
  void Destroy();

  // Copies from the GPU texture object to the staging texture, which can be mapped/read by the CPU.
  // Both src_rect and dst_rect must be with within the bounds of the the specified textures.
  void CopyFromTexture(GL::Texture& src_texture, u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 dst_x,
                       u32 dst_y, u32 width, u32 height);

  // Wrapper for copying a whole layer of a texture to a readback texture.
  // Assumes that the level of src texture and this texture have the same dimensions.
  void CopyToTexture(u32 src_x, u32 src_y, GL::Texture& dst_texture, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                     u32 width, u32 height);

  bool Map();
  void Unmap();

  // Flushes pending writes from the CPU to the GPU, and reads from the GPU to the CPU.
  // This may cause a command buffer flush depending on if one has occurred between the last
  // call to CopyFromTexture()/CopyToTexture() and the Flush() call.
  void Flush();

  // Reads the specified rectangle from the staging texture to out_ptr, with the specified stride
  // (length in bytes of each row). CopyFromTexture must be called first. The contents of any
  // texels outside of the rectangle used for CopyFromTexture is undefined.
  void ReadTexels(u32 src_x, u32 src_y, u32 width, u32 height, void* out_ptr, u32 out_stride);
  void ReadTexel(u32 x, u32 y, void* out_ptr);

  // Copies the texels from in_ptr to the staging texture, which can be read by the GPU, with the
  // specified stride (length in bytes of each row). After updating the staging texture with all
  // changes, call CopyToTexture() to update the GPU copy.
  void WriteTexels(u32 dst_x, u32 dst_y, u32 width, u32 height, const void* in_ptr, u32 in_stride);
  void WriteTexel(u32 x, u32 y, const void* in_ptr);

private:
  ALWAYS_INLINE GLenum GetTarget() const { return m_readback ? GL_PIXEL_UNPACK_BUFFER : GL_PIXEL_PACK_BUFFER; }

  void PrepareForAccess();

  u32 m_width = 0;
  u32 m_height = 0;
  GLenum m_format = GL_RGBA8;

  GLuint m_buffer_name = 0;
  u32 m_buffer_size = 0;
  GLsync m_fence = 0;

  char* m_map_pointer = nullptr;
  u32 m_stride = 0;
  u32 m_texel_size = 0;
  bool m_readback = false;
  bool m_needs_flush = false;
};

} // namespace GL