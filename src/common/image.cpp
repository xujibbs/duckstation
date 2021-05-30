#include "image.h"
#include "byte_stream.h"
#include "file_system.h"
#include "log.h"
#include "stb_image.h"
#include "stb_image_resize.h"
#include "stb_image_write.h"
#include "string_util.h"
Log_SetChannel(Common::Image);

namespace Common {
bool LoadImageFromFile(Common::RGBA8Image* image, const char* filename)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "rb");
  if (!fp)
  {
    return false;
  }

  int width, height, file_channels;
  u8* pixel_data = stbi_load_from_file(fp.get(), &width, &height, &file_channels, 4);
  if (!pixel_data)
  {
    const char* error_reason = stbi_failure_reason();
    Log_ErrorPrintf("Failed to load image from '%s': %s", filename, error_reason ? error_reason : "unknown error");
    return false;
  }

  image->SetPixels(static_cast<u32>(width), static_cast<u32>(height), reinterpret_cast<const u32*>(pixel_data));
  stbi_image_free(pixel_data);
  return true;
}

bool LoadImageFromBuffer(Common::RGBA8Image* image, const void* buffer, std::size_t buffer_size)
{
  int width, height, file_channels;
  u8* pixel_data = stbi_load_from_memory(static_cast<const stbi_uc*>(buffer), static_cast<int>(buffer_size), &width,
                                         &height, &file_channels, 4);
  if (!pixel_data)
  {
    const char* error_reason = stbi_failure_reason();
    Log_ErrorPrintf("Failed to load image from memory: %s", error_reason ? error_reason : "unknown error");
    return false;
  }

  image->SetPixels(static_cast<u32>(width), static_cast<u32>(height), reinterpret_cast<const u32*>(pixel_data));
  stbi_image_free(pixel_data);
  return true;
}

bool LoadImageFromStream(RGBA8Image* image, ByteStream* stream)
{
  stbi_io_callbacks iocb;
  iocb.read = [](void* user, char* data, int size) {
    return static_cast<int>(static_cast<ByteStream*>(user)->Read(data, static_cast<u32>(size)));
  };
  iocb.skip = [](void* user, int n) { static_cast<ByteStream*>(user)->SeekRelative(n); };
  iocb.eof = [](void* user) {
    ByteStream* stream = static_cast<ByteStream*>(user);
    return (stream->InErrorState() || stream->GetPosition() == stream->GetSize()) ? 1 : 0;
  };

  int width, height, file_channels;
  u8* pixel_data = stbi_load_from_callbacks(&iocb, stream, &width, &height, &file_channels, 4);
  if (!pixel_data)
  {
    const char* error_reason = stbi_failure_reason();
    Log_ErrorPrintf("Failed to load image from stream: %s", error_reason ? error_reason : "unknown error");
    return false;
  }

  image->SetPixels(static_cast<u32>(width), static_cast<u32>(height), reinterpret_cast<const u32*>(pixel_data));
  stbi_image_free(pixel_data);
  return true;
}

bool WriteImageToFile(const RGBA8Image& image, const char* filename)
{
  const char* extension = std::strrchr(filename, '.');
  if (!extension)
  {
    Log_ErrorPrintf("Unable to determine file extension for '%s'", filename);
    return false;
  }

  auto fp = FileSystem::OpenManagedCFile(filename, "wb");
  if (!fp)
    return {};

  const auto write_func = [](void* context, void* data, int size) {
    std::fwrite(data, 1, size, static_cast<std::FILE*>(context));
  };

  bool result = false;
  if (StringUtil::Strcasecmp(extension, ".png") == 0)
  {
    result = (stbi_write_png_to_func(write_func, fp.get(), image.GetWidth(), image.GetHeight(), 4, image.GetPixels(),
                                     image.GetByteStride()) != 0);
  }
  else if (StringUtil::Strcasecmp(extension, ".jpg") == 0)
  {
    result = (stbi_write_jpg_to_func(write_func, fp.get(), image.GetWidth(), image.GetHeight(), 4, image.GetPixels(),
                                     95) != 0);
  }
  else if (StringUtil::Strcasecmp(extension, ".tga") == 0)
  {
    result =
      (stbi_write_tga_to_func(write_func, fp.get(), image.GetWidth(), image.GetHeight(), 4, image.GetPixels()) != 0);
  }
  else if (StringUtil::Strcasecmp(extension, ".bmp") == 0)
  {
    result =
      (stbi_write_bmp_to_func(write_func, fp.get(), image.GetWidth(), image.GetHeight(), 4, image.GetPixels()) != 0);
  }

  if (!result)
  {
    Log_ErrorPrintf("Unknown extension in filename '%s' or save error: '%s'", filename, extension);
    return false;
  }

  return true;
}

void ResizeImage(RGBA8Image* image, u32 new_width, u32 new_height)
{
  if (image->GetWidth() == new_width && image->GetHeight() == new_height)
    return;

  std::vector<u32> resized_texture_data(new_width * new_height);
  u32 resized_texture_stride = sizeof(u32) * new_width;
  if (!stbir_resize_uint8(reinterpret_cast<u8*>(image->GetPixels()), image->GetWidth(), image->GetHeight(),
                          image->GetByteStride(), reinterpret_cast<u8*>(resized_texture_data.data()), new_width,
                          new_height, resized_texture_stride, 4))
  {
    Panic("stbir_resize_uint8 failed");
    return;
  }

  image->SetPixels(new_width, new_height, std::move(resized_texture_data));
}

void ResizeImage(RGBA8Image* dst_image, const RGBA8Image* src_image, u32 new_width, u32 new_height)
{
  if (src_image->GetWidth() == new_width && src_image->GetHeight() == new_height)
  {
    dst_image->SetPixels(src_image->GetWidth(), src_image->GetHeight(), src_image->GetPixels());
    return;
  }

  dst_image->SetSize(new_width, new_height);
  if (!stbir_resize_uint8(reinterpret_cast<const u8*>(src_image->GetPixels()), src_image->GetWidth(),
                          src_image->GetHeight(), src_image->GetByteStride(),
                          reinterpret_cast<u8*>(dst_image->GetPixels()), dst_image->GetWidth(), dst_image->GetHeight(),
                          dst_image->GetByteStride(), 4))
  {
    Panic("stbir_resize_uint8 failed");
    return;
  }
}

} // namespace Common