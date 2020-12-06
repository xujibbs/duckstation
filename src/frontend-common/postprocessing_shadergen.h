#pragma once
#include "core/shadergen.h"
#include "postprocessing_shader.h"
#include <sstream>

namespace FrontendCommon {

class PostProcessingShaderGen : public ShaderGen
{
public:
  PostProcessingShaderGen(HostDisplay::RenderAPI render_api, bool supports_dual_source_blend);
  ~PostProcessingShaderGen();

  std::string GeneratePostProcessingVertexShader(const PostProcessingShader& shader, u32 pass);
  std::string GeneratePostProcessingFragmentShader(const PostProcessingShader& shader, u32 pass);

private:
  void WriteUniformBuffer(std::stringstream& ss, const PostProcessingShader& shader, bool use_push_constants);

  std::string GenerateLegacyPostProcessingVertexShader(const PostProcessingShader& shader, u32 pass);
  std::string GenerateLegacyPostProcessingFragmentShader(const PostProcessingShader& shader, u32 pass);
};

} // namespace FrontendCommon