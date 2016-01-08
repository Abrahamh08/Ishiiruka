// Copyright 2009 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Common/Common.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

#include "Common/GL/GLUtil.h"

#include "VideoBackends/OGL/FramebufferManager.h"
#include "VideoBackends/OGL/PostProcessing.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"
#include "VideoBackends/OGL/Render.h"
#include "VideoBackends/OGL/SamplerCache.h"
#include "VideoBackends/OGL/TextureCache.h"

#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{

static const u32 FIRST_INPUT_TEXTURE_UNIT = 9;
static const u32 UNIFORM_BUFFER_BIND_POINT = 4;

static const char* s_vertex_shader = R"(
out vec2 uv0;
flat out float layer;
void main(void)
{
vec2 rawpos = vec2(gl_VertexID&1, gl_VertexID&2);
gl_Position = vec4(rawpos*2.0-1.0, 0.0, 1.0);
uv0 = rawpos * src_rect.zw + src_rect.xy;
layer = src_layer;
}
)";

static const char* s_layered_vertex_shader = R"(
out vec2 v_uv0;
void main(void)
{
vec2 rawpos = vec2(gl_VertexID&1, gl_VertexID&2);
gl_Position = vec4(rawpos*2.0-1.0, 0.0, 1.0);
v_uv0 = rawpos * src_rect.zw + src_rect.xy;
}
)";

static const char* s_geometry_shader = R"(

	layout(triangles) in;
layout(triangle_strip, max_vertices = %d) out;

in vec2 v_uv0[3];
out vec2 uv0;
flat out float layer;

void main()
{
for (int i = 0; i < %d; i++)
{
	for (int j = 0; j < 3; j++)
	{
		gl_Position = gl_in[j].gl_Position;
		uv0 = v_uv0[j];
		layer = float(i);
		gl_Layer = i;
		EmitVertex();
	}

			EndPrimitive();
}
}

)";

PostProcessingShader::~PostProcessingShader()
{
	// Delete texture objects that we own
	for (RenderPassData& pass : m_passes)
	{
		for (InputBinding& input : pass.inputs)
		{
			// External textures
			if (input.texture_id != 0 && input.owned)
			{
				glDeleteTextures(1, &input.texture_id);
				input.texture_id = 0;
			}
		}

		if (pass.program != nullptr)
		{
			pass.program->Destroy();
			pass.program.reset();
		}

		if (pass.output_texture_id != 0)
		{
			glDeleteTextures(1, &pass.output_texture_id);
			pass.output_texture_id = 0;
		}
	}

	if (m_framebuffer != 0)
		glDeleteFramebuffers(1, &m_framebuffer);
}

bool PostProcessingShader::Initialize(const PostProcessingShaderConfiguration* config, int target_layers)
{
	m_internal_layers = target_layers;
	m_config = config;
	m_ready = false;

	// In case we need to allocate texture objects
	glActiveTexture(GL_TEXTURE0 + FIRST_INPUT_TEXTURE_UNIT);

	m_passes.reserve(m_config->GetPasses().size());
	for (const auto& pass_config : m_config->GetPasses())
	{
		RenderPassData pass;
		pass.output_texture_id = 0;
		pass.output_width = 0;
		pass.output_height = 0;
		pass.output_scale = pass_config.output_scale;
		pass.enabled = true;

		pass.inputs.reserve(pass_config.inputs.size());
		for (const auto& input_config : pass_config.inputs)
		{
			// Non-external textures will be bound at run-time.
			InputBinding input;
			input.type = input_config.type;
			input.texture_unit = input_config.texture_unit;
			input.texture_id = 0;
			input.sampler_id = 0;
			input.width = 1;
			input.height = 1;
			input.owned = false;

			// Only external images have to be set up here
			if (input.type == POST_PROCESSING_INPUT_TYPE_IMAGE)
			{
				_dbg_assert_(VIDEO, input_config.external_image_width > 0 && input_config.external_image_height > 0);
				input.width = input_config.external_image_width;
				input.height = input_config.external_image_height;
				input.owned = true;

				// Copy the image across all layers
				glGenTextures(1, &input.texture_id);
				glActiveTexture(GL_TEXTURE0 + FIRST_INPUT_TEXTURE_UNIT);
				glBindTexture(GL_TEXTURE_2D_ARRAY, input.texture_id);
				glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, input.width, input.height, target_layers, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
				glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
				for (int layer = 0; layer < target_layers; layer++)
					glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, input.width, input.height, 1, GL_RGBA, GL_UNSIGNED_BYTE, input_config.external_image_data.get());
			}

			// Lookup tables for samplers, simple due to no mipmaps
			static const GLenum gl_sampler_filters[] = { GL_NEAREST, GL_LINEAR };
			static const GLenum gl_sampler_modes[] = { GL_CLAMP_TO_EDGE, GL_REPEAT,  GL_CLAMP_TO_BORDER };
			static const float gl_border_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

			// Create sampler object matching the values from config
			glGenSamplers(1, &input.sampler_id);
			glSamplerParameteri(input.sampler_id, GL_TEXTURE_MIN_FILTER, gl_sampler_filters[input_config.filter]);
			glSamplerParameteri(input.sampler_id, GL_TEXTURE_MAG_FILTER, gl_sampler_filters[input_config.filter]);
			glSamplerParameteri(input.sampler_id, GL_TEXTURE_WRAP_S, gl_sampler_modes[input_config.address_mode]);
			glSamplerParameteri(input.sampler_id, GL_TEXTURE_WRAP_T, gl_sampler_modes[input_config.address_mode]);
			glSamplerParameterfv(input.sampler_id, GL_TEXTURE_BORDER_COLOR, gl_border_color);

			pass.inputs.push_back(std::move(input));
		}
		m_passes.push_back(std::move(pass));
	}

	// Allocate framebuffer object
	glGenFramebuffers(1, &m_framebuffer);
	if (m_framebuffer == 0)
	{
		ERROR_LOG(VIDEO, "Failed to create FBO");
		TextureCache::SetStage();
		return false;
	}
	
	// Restore texture state
	TextureCache::SetStage();
	
	// Compile shaders
	if (!RecompileShaders())
		return false;

	// Determine which passes to execute
	UpdateEnabledPasses();

	// In case we created any textures
	TextureCache::SetStage();
	m_ready = true;
	return true;
}

bool PostProcessingShader::ResizeIntermediateBuffers(int target_width, int target_height)
{
	_dbg_assert_(VIDEO, target_width > 0 && target_height > 0);
	if (m_internal_width == target_width && m_internal_height == target_height)
		return true;

	m_ready = false;

	glActiveTexture(GL_TEXTURE0 + FIRST_INPUT_TEXTURE_UNIT);

	size_t previous_pass = 0;
	for (size_t pass_index = 0; pass_index < m_passes.size(); pass_index++)
	{
		RenderPassData& pass = m_passes[pass_index];
		const PostProcessingShaderConfiguration::RenderPass& pass_config = m_config->GetPass(pass_index);
		PostProcessor::ScaleTargetSize(&pass.output_width, &pass.output_height, target_width, target_height, pass_config.output_scale);

		// Re-use existing texture object if one already exists
		if (pass.output_texture_id == 0)
			glGenTextures(1, &pass.output_texture_id);

		glBindTexture(GL_TEXTURE_2D_ARRAY, pass.output_texture_id);
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, pass.output_width, pass.output_height, m_internal_layers, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);

		// Hook up any inputs that are other passes
		for (size_t input_index = 0; input_index < pass_config.inputs.size(); input_index++)
		{
			const PostProcessingShaderConfiguration::RenderPass::Input& input_config = pass_config.inputs[input_index];
			InputBinding& input_binding = pass.inputs[input_index];
			if (input_config.type == POST_PROCESSING_INPUT_TYPE_PASS_OUTPUT)
			{
				_dbg_assert_(VIDEO, input_config.pass_output_index < pass_index);
				input_binding.texture_id = m_passes[input_config.pass_output_index].output_texture_id;
				input_binding.width = m_passes[input_config.pass_output_index].output_width;
				input_binding.height = m_passes[input_config.pass_output_index].output_height;
			}
			else if (input_config.type == POST_PROCESSING_INPUT_TYPE_PREVIOUS_PASS_OUTPUT)
			{
				_dbg_assert_(VIDEO, previous_pass < pass_index);
				input_binding.texture_id = m_passes[previous_pass].output_texture_id;
				input_binding.width = m_passes[previous_pass].output_width;
				input_binding.height = m_passes[previous_pass].output_height;
			}
		}

		if (pass.enabled)
			previous_pass = pass_index;
	}

	TextureCache::SetStage();
	m_internal_width = target_width;
	m_internal_height = target_height;
	m_ready = true;
	return true;
}

void PostProcessingShader::Draw(OGLPostProcessor* parent,
	const TargetRectangle& target_rect, GLuint target_texture,
	const TargetRectangle& src_rect, int src_width, int src_height,
	GLuint src_texture, GLuint src_depth_texture,
	int src_layer, float gamma)
{
	_dbg_assert_(VIDEO, m_ready);
	OpenGL_BindAttributelessVAO();

	// Determine whether we can skip the final copy by writing directly to the output texture.
	bool skip_final_copy = (target_texture != src_texture || !m_last_pass_uses_color_buffer);

	// If the last pass is not at full scale, we can't skip the copy.
	if (m_passes[m_last_pass_index].output_width != src_width || m_passes[m_last_pass_index].output_height != src_height)
		skip_final_copy = false;

	// Draw each pass.
	TargetRectangle output_rect = {};
	int input_resolutions[POST_PROCESSING_MAX_TEXTURE_INPUTS][2] = {};
	for (size_t pass_index = 0; pass_index < m_passes.size(); pass_index++)
	{
		const RenderPassData& pass = m_passes[pass_index];
		bool is_last_pass = (pass_index == m_last_pass_index);

		// If this is the last pass and we can skip the final copy, write directly to output texture.
		GLuint output_texture;
		if (is_last_pass && skip_final_copy)
		{
			output_rect = target_rect;
			output_texture = target_texture;
		}
		else
		{
			output_rect = PostProcessor::ScaleTargetRectangle(API_OPENGL, src_rect, pass.output_scale);
			output_texture = pass.output_texture_id;
		}

		// Setup framebuffer
		if (output_texture != 0)
		{
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_framebuffer);
			if (src_layer < 0 && m_internal_layers > 1)
				glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, output_texture, 0);
			else if (src_layer >= 0)
				glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, output_texture, 0, src_layer);
			else
				glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, output_texture, 0, 0);
		}
		else
		{
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		}

		// Bind program and texture units here
		if (src_layer < 0 && m_internal_layers > 1)
			pass.gs_program->Bind();
		else
			pass.program->Bind();

		for (size_t i = 0; i < pass.inputs.size(); i++)
		{
			const InputBinding& input = pass.inputs[i];
			glActiveTexture(GL_TEXTURE0 + FIRST_INPUT_TEXTURE_UNIT + input.texture_unit);

			switch (input.type)
			{
			case POST_PROCESSING_INPUT_TYPE_COLOR_BUFFER:
				glBindTexture(GL_TEXTURE_2D_ARRAY, src_texture);
				input_resolutions[i][0] = src_width;
				input_resolutions[i][1] = src_height;
				break;

			case POST_PROCESSING_INPUT_TYPE_DEPTH_BUFFER:
				glBindTexture(GL_TEXTURE_2D_ARRAY, src_depth_texture);
				input_resolutions[i][0] = src_width;
				input_resolutions[i][1] = src_height;
				break;

			default:
				glBindTexture(GL_TEXTURE_2D_ARRAY, input.texture_id);
				input_resolutions[i][0] = input.width;
				input_resolutions[i][1] = input.height;
				break;
			}

			glBindSampler(FIRST_INPUT_TEXTURE_UNIT + input.texture_unit, input.sampler_id);
		}

		parent->MapAndUpdateUniformBuffer(m_config, input_resolutions, src_rect, target_rect, src_width, src_height, src_layer, gamma);
		glViewport(output_rect.left, output_rect.bottom, output_rect.GetWidth(), output_rect.GetHeight());
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}

	// Copy the last pass output to the target if not done already
	if (!skip_final_copy)
		parent->CopyTexture(target_rect, target_texture, output_rect, m_passes[m_last_pass_index].output_texture_id, src_layer, false);
}

bool PostProcessingShader::RecompileShaders()
{
	for (size_t i = 0; i < m_passes.size(); i++)
	{
		RenderPassData& pass = m_passes[i];
		const PostProcessingShaderConfiguration::RenderPass& pass_config = m_config->GetPass(i);

		// Compile shader for this pass
		std::unique_ptr<SHADER> program = std::make_unique<SHADER>();
		std::string vertex_shader_source = PostProcessor::GetUniformBufferShaderSource(API_OPENGL, m_config) + s_vertex_shader;
		std::string fragment_shader_source = PostProcessor::GetPassFragmentShaderSource(API_OPENGL, m_config, &pass_config);
		if (!ProgramShaderCache::CompileShader(*program, vertex_shader_source.c_str(), fragment_shader_source.c_str()))
		{
			ERROR_LOG(VIDEO, "Failed to compile post-processing shader %s (pass %s)", m_config->GetShader().c_str(), pass_config.entry_point.c_str());
			m_ready = false;
			return false;
		}

		// Bind our uniform block
		GLuint block_index = glGetUniformBlockIndex(program->glprogid, "PostProcessingConstants");
		if (block_index != GL_INVALID_INDEX)
			glUniformBlockBinding(program->glprogid, block_index, UNIFORM_BUFFER_BIND_POINT);

		// Only generate a GS-expanding program if needed
		std::unique_ptr<SHADER> gs_program;
		if (m_internal_layers > 1)
		{
			gs_program = std::make_unique<SHADER>();
			vertex_shader_source = PostProcessor::GetUniformBufferShaderSource(API_OPENGL, m_config) + s_layered_vertex_shader;
			std::string geometry_shader_source = StringFromFormat(s_geometry_shader, m_internal_layers * 3, m_internal_layers).c_str();

			if (!ProgramShaderCache::CompileShader(*gs_program, vertex_shader_source.c_str(), fragment_shader_source.c_str(), geometry_shader_source.c_str()))
			{
				ERROR_LOG(VIDEO, "Failed to compile GS post-processing shader %s (pass %s)", m_config->GetShader().c_str(), pass_config.entry_point.c_str());
				m_ready = false;
				return false;
			}

			block_index = glGetUniformBlockIndex(gs_program->glprogid, "PostProcessingConstants");
			if (block_index != GL_INVALID_INDEX)
				glUniformBlockBinding(gs_program->glprogid, block_index, UNIFORM_BUFFER_BIND_POINT);
		}

		// Store to struct
		std::swap(pass.program, program);
		std::swap(pass.gs_program, gs_program);
	}

	return true;
}

void PostProcessingShader::UpdateEnabledPasses()
{
	m_last_pass_index = 0;
	m_last_pass_uses_color_buffer = false;

	for (size_t pass_index = 0; pass_index < m_passes.size(); pass_index++)
	{
		const PostProcessingShaderConfiguration::RenderPass& pass_config = m_config->GetPass(pass_index);
		RenderPassData& pass = m_passes[pass_index];
		pass.enabled = pass_config.CheckEnabled();

		// Check for color buffer reads, for copy optimization
		if (pass.enabled)
		{
			m_last_pass_index = pass_index;
			m_last_pass_uses_color_buffer = false;
			for (size_t input_index = 0; input_index < pass.inputs.size(); input_index++)
			{
				InputBinding& input = pass.inputs[input_index];
				if (input.type == POST_PROCESSING_INPUT_TYPE_COLOR_BUFFER)
				{
					m_last_pass_uses_color_buffer = true;
					break;
				}
			}
		}
	}
}

OGLPostProcessor::~OGLPostProcessor()
{
	if (m_read_framebuffer != 0)
		glDeleteFramebuffers(1, &m_read_framebuffer);
	if (m_draw_framebuffer != 0)
		glDeleteFramebuffers(1, &m_draw_framebuffer);
	if (m_color_copy_texture != 0)
		glDeleteTextures(1, &m_color_copy_texture);
	if (m_depth_copy_texture != 0)
		glDeleteTextures(1, &m_depth_copy_texture);
}

bool OGLPostProcessor::Initialize()
{
	// Create our framebuffer objects, since these are needed regardless of whether we're enabled.
	glGenFramebuffers(1, &m_draw_framebuffer);
	glGenFramebuffers(1, &m_read_framebuffer);
	if (glGetError() != GL_NO_ERROR)
	{
		ERROR_LOG(VIDEO, "Failed to create postprocessing framebuffer objects.");
		return false;
	}

	m_uniform_buffer = StreamBuffer::Create(GL_UNIFORM_BUFFER, UNIFORM_BUFFER_SIZE * 16);
	if (m_uniform_buffer == nullptr)
	{
		ERROR_LOG(VIDEO, "Failed to create postprocessing uniform buffer.");
		return false;
	}

	// Allocate copy texture names, the actual storage is done in ResizeCopyBuffers
	glGenTextures(1, &m_color_copy_texture);
	glGenTextures(1, &m_depth_copy_texture);
	if (m_color_copy_texture == 0 || m_depth_copy_texture == 0)
	{
		ERROR_LOG(VIDEO, "Failed to create copy textures.");
		return false;
	}

	// Load the currently-configured shader (this may fail, and that's okay)
	ReloadShaders();
	return true;
}

void OGLPostProcessor::ReloadShaders()
{
	// Delete current shaders
	m_reload_flag.Clear();
	m_post_processing_shader.reset();
	m_blit_shader.reset();
	m_active = false;

	if (g_ActiveConfig.bPostProcessingEnable)
	{
		// Load current shader, and create program
		const std::string& post_shader_name = g_ActiveConfig.sPostProcessingShader;
		m_post_processing_shader = std::make_unique<PostProcessingShader>();
		if (m_config.LoadShader("", post_shader_name) && m_post_processing_shader->Initialize(&m_config, FramebufferManager::GetEFBLayers()))
		{
			if (!post_shader_name.empty())
			{
				DEBUG_LOG(VIDEO, "Postprocessing shader loaded: '%s'", post_shader_name.c_str());
				OSD::AddMessage(StringFromFormat("Postprocessing shader loaded: '%s'", post_shader_name.c_str()));
			}

			m_config.ClearDirty();
			m_active = true;
		}
		else
		{
			ERROR_LOG(VIDEO, "Failed to load postprocessing shader ('%s'). Disabling post processor.", post_shader_name.c_str());
			OSD::AddMessage(StringFromFormat("Failed to load postprocessing shader ('%s'). Disabling post processor.", post_shader_name.c_str()));

			m_post_processing_shader.reset();
		}
	}

	const std::string& blit_shader_subdir = (g_ActiveConfig.iStereoMode == STEREO_ANAGLYPH) ? ANAGLYPH_DIR : "";
	const std::string& blit_shader_name = (g_ActiveConfig.iStereoMode == STEREO_ANAGLYPH) ? g_ActiveConfig.sAnaglyphShader : g_ActiveConfig.sBlitShader;
	m_blit_shader = std::make_unique<PostProcessingShader>();
	if (m_blit_config.LoadShader(blit_shader_subdir, blit_shader_name) && m_blit_shader->Initialize(&m_blit_config, FramebufferManager::GetEFBLayers()))
	{
		if (!blit_shader_name.empty())
		{
			DEBUG_LOG(VIDEO, "Blit shader loaded: '%s'", blit_shader_name.c_str());
			OSD::AddMessage(StringFromFormat("Blit shader loaded: '%s'", blit_shader_name.c_str()));
		}

		m_blit_config.ClearDirty();
	}
	else
	{
		ERROR_LOG(VIDEO, "Failed to load blit shader ('%s'). Falling back to glBlitFramebuffer().", blit_shader_name.c_str());
		OSD::AddMessage(StringFromFormat("Failed to load blit shader ('%s'). Falling back to glBlitFramebuffer().", blit_shader_name.c_str()));

		m_blit_shader.reset();
	}
}

void OGLPostProcessor::PostProcessEFB()
{
	// Uses the current viewport as the "visible" region to post-process.
	g_renderer->ResetAPIState();

	int scissorXOff = bpmem.scissorOffset.x * 2;
	int scissorYOff = bpmem.scissorOffset.y * 2;
	float X = Renderer::EFBToScaledXf(xfmem.viewport.xOrig - xfmem.viewport.wd - (float)scissorXOff);
	float Y = Renderer::EFBToScaledYf((float)EFB_HEIGHT - xfmem.viewport.yOrig + xfmem.viewport.ht + (float)scissorYOff);
	float Width = Renderer::EFBToScaledXf(2.0f * xfmem.viewport.wd);
	float Height = Renderer::EFBToScaledYf(-2.0f * xfmem.viewport.ht);
	if (Width < 0)
	{
		X += Width;
		Width *= -1;
	}
	if (Height < 0)
	{
		Y += Height;
		Height *= -1;
	}

	EFBRectangle efb_rect(0, EFB_HEIGHT, EFB_WIDTH, 0);
	TargetRectangle target_rect(static_cast<int>(X), static_cast<int>(Y + Height),
		static_cast<int>(X + Width), static_cast<int>(Y));

	// Source and target textures, if MSAA is enabled, this needs to be resolved
	GLuint efb_color_texture = FramebufferManager::GetEFBColorTexture(efb_rect);
	GLuint efb_depth_texture = 0;
	if (m_config.RequiresDepthBuffer())
		efb_depth_texture = FramebufferManager::GetEFBDepthTexture(efb_rect);

	// Invoke post-process process, this will write back to efb_color_texture
	PostProcess(target_rect, g_renderer->GetTargetWidth(), g_renderer->GetTargetHeight(),
		FramebufferManager::GetEFBLayers(), efb_color_texture, efb_depth_texture);

	// Restore EFB framebuffer
	FramebufferManager::SetFramebuffer(0);

	// In msaa mode, we need to blit back to the original framebuffer.
	// An accessor for the texture name means we could use CopyTexture here.
	if (g_ActiveConfig.iMultisamples > 1)
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_read_framebuffer);
		FramebufferManager::FramebufferTexture(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D_ARRAY, efb_color_texture, 0);

		glBlitFramebuffer(target_rect.left, target_rect.bottom, target_rect.right, target_rect.top,
			target_rect.left, target_rect.bottom, target_rect.right, target_rect.top,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	}

	g_renderer->RestoreAPIState();
}

void OGLPostProcessor::BlitToFramebuffer(const TargetRectangle& dst, uintptr_t dst_texture,
	const TargetRectangle& src, uintptr_t src_texture,
	int src_width, int src_height, int src_layer, float gamma)
{
	GLuint real_dst_texture = static_cast<GLuint>(dst_texture);
	GLuint real_src_texture = static_cast<GLuint>(src_texture);
	_dbg_assert_msg_(VIDEO, src_layer >= 0, "BlitToFramebuffer should always be called with a single source layer");

	// Options changed?
	if (m_blit_shader != nullptr && m_blit_shader->IsReady())
	{
		if (m_blit_config.IsDirty())
		{
			if (m_blit_config.IsCompileTimeConstantsDirty())
				m_blit_shader->RecompileShaders();

			m_blit_shader->UpdateEnabledPasses();
			m_blit_config.ClearDirty();
		}
		m_blit_shader->ResizeIntermediateBuffers(src_width, src_height);
	}

	// Use blit shader if one is set-up. Should only be a single pass in almost all cases.
	if (m_blit_shader != nullptr && m_blit_shader->IsReady())
	{
		m_blit_shader->Draw(this, dst, real_dst_texture, src, src_width, src_height, real_src_texture, 0, src_layer, gamma);
	}
	else
	{
		CopyTexture(dst, real_dst_texture, src, real_src_texture, src_layer, false);
	}
}

void OGLPostProcessor::PostProcess(const TargetRectangle& visible_rect, int tex_width, int tex_height, int tex_layers,
	uintptr_t texture, uintptr_t depth_texture)
{
	GLuint real_texture = static_cast<GLuint>(texture);
	GLuint real_depth_texture = static_cast<GLuint>(depth_texture);
	_dbg_assert_(VIDEO, !m_active || m_post_processing_shader != nullptr);
	if (!m_active)
		return;

	int visible_width = visible_rect.GetWidth();
	int visible_height = visible_rect.GetHeight();
	if (!m_post_processing_shader->IsReady() ||
		!m_post_processing_shader->ResizeIntermediateBuffers(visible_width, visible_height) ||
		!ResizeCopyBuffers(visible_width, visible_height, tex_layers) ||
		(m_config.IsCompileTimeConstantsDirty() && !m_post_processing_shader->RecompileShaders()))
	{
		ERROR_LOG(VIDEO, "Failed to create post-process intermediate buffers. Disabling post processor.");
		m_post_processing_shader.reset();
		m_active = false;
		return;
	}

	if (m_config.IsDirty())
	{
		m_post_processing_shader->UpdateEnabledPasses();
		m_config.ClearDirty();
	}

	// Copy the visible region to our buffers.
	TargetRectangle buffer_rect(0, visible_height, visible_width, 0);
	CopyTexture(buffer_rect, m_color_copy_texture, visible_rect, real_texture, -1, false);
	if (real_depth_texture != 0)
		CopyTexture(buffer_rect, m_depth_copy_texture, visible_rect, real_depth_texture, -1, false);

	m_post_processing_shader->Draw(this, visible_rect, real_texture, buffer_rect, visible_width, visible_height,
		m_color_copy_texture, m_depth_copy_texture, -1);
}

void OGLPostProcessor::MapAndUpdateUniformBuffer(const PostProcessingShaderConfiguration* config,
	int input_resolutions[POST_PROCESSING_MAX_TEXTURE_INPUTS][2],
	const TargetRectangle& src_rect, const TargetRectangle& dst_rect, int src_width, int src_height, int src_layer, float gamma)
{
	std::pair<u8*, u32> ubo = m_uniform_buffer->Map(UNIFORM_BUFFER_SIZE, UNIFORM_BUFFER_SIZE);
	UpdateUniformBuffer(API_OPENGL, config, ubo.first, input_resolutions, src_rect, dst_rect, src_width, src_height, src_layer, gamma);
	m_uniform_buffer->Unmap(UNIFORM_BUFFER_SIZE);
	glBindBufferRange(GL_UNIFORM_BUFFER, UNIFORM_BUFFER_BIND_POINT, m_uniform_buffer->m_buffer, ubo.second, UNIFORM_BUFFER_SIZE);
}

void OGLPostProcessor::CopyTexture(const TargetRectangle& dst_rect, GLuint dst_texture,
	const TargetRectangle& src_rect, GLuint src_texture,
	int src_layer, bool is_depth_texture,
	bool force_blit)
{
	// Can we copy the image?
	bool scaling = (dst_rect.GetWidth() != src_rect.GetWidth() || dst_rect.GetHeight() != src_rect.GetHeight());
	int layers_to_copy = (src_layer < 0) ? FramebufferManager::GetEFBLayers() : 1;

	// Copy each layer individually.
	for (int i = 0; i < layers_to_copy; i++)
	{
		int layer = (src_layer < 0) ? i : src_layer;
		if (g_ogl_config.bSupportsCopySubImage && dst_texture != 0 && !force_blit)
		{
			// use (ARB|NV)_copy_image, but only for non-window-framebuffer cases
			glCopyImageSubData(src_texture, GL_TEXTURE_2D_ARRAY, 0, src_rect.left, src_rect.bottom, layer,
				dst_texture, GL_TEXTURE_2D_ARRAY, 0, dst_rect.left, dst_rect.bottom, layer,
				src_rect.GetWidth(), src_rect.GetHeight(), 1);
		}
		else
		{
			// fallback to glBlitFramebuffer path
			GLenum filter = (scaling) ? GL_LINEAR : GL_NEAREST;
			GLbitfield bits = (!is_depth_texture) ? GL_COLOR_BUFFER_BIT : GL_DEPTH_BUFFER_BIT;
			if (dst_texture != 0)
			{
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_draw_framebuffer);
				if (!is_depth_texture)
					glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, dst_texture, 0, layer);
				else
					glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, dst_texture, 0, layer);
			}
			else
			{
				// window framebuffer
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
			}

			glBindFramebuffer(GL_READ_FRAMEBUFFER, m_read_framebuffer);
			if (!is_depth_texture)
				glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, src_texture, 0, layer);
			else
				glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, src_texture, 0, layer);

			glBlitFramebuffer(src_rect.left, src_rect.bottom, src_rect.right, src_rect.top,
				dst_rect.left, dst_rect.bottom, dst_rect.right, dst_rect.top,
				bits, filter);
		}
	}
}

bool OGLPostProcessor::ResizeCopyBuffers(int width, int height, int layers)
{
	if (m_copy_width == width && m_copy_height == height && m_copy_layers == layers)
		return true;

	m_copy_width = width;
	m_copy_height = height;
	m_copy_layers = layers;

	glActiveTexture(GL_TEXTURE0 + FIRST_INPUT_TEXTURE_UNIT);
	glBindTexture(GL_TEXTURE_2D_ARRAY, m_color_copy_texture);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, width, height, FramebufferManager::GetEFBLayers(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glBindTexture(GL_TEXTURE_2D_ARRAY, m_depth_copy_texture);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT32F, width, height, FramebufferManager::GetEFBLayers(), 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, nullptr);

	TextureCache::SetStage();
	return true;
}

}  // namespace OGL
