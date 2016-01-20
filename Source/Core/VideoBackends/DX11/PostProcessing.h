// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <d3d11.h>
#include <string>
#include <unordered_map>
#include <wrl/client.h>

#include "VideoBackends/DX11/D3DTexture.h"
#include "VideoBackends/DX11/D3DPtr.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/VideoCommon.h"

namespace DX11
{

// Forward declaration needed for PostProcessingShader::Draw()
class D3DPostProcessor;

// PostProcessingShader comprises of all the resources needed to render a shader, including
// temporary buffers, external images, shader programs, and configurations.
class PostProcessingShader final
{
public:
	PostProcessingShader() = default;
	~PostProcessingShader();

	D3DTexture2D* GetLastPassOutputTexture() const;
	bool IsLastPassScaled() const;

	bool IsReady() const { return m_ready; }

	bool Initialize(const PostProcessingShaderConfiguration* config, int target_layers);
	bool Reconfigure(const TargetSize& new_size);

	void Draw(D3DPostProcessor* parent,
		const TargetRectangle& dst_rect, const TargetSize& dst_size, D3DTexture2D* dst_texture,
		const TargetRectangle& src_rect, const TargetSize& src_size, D3DTexture2D* src_texture,
		D3DTexture2D* src_depth_texture, int src_layer, float gamma);

private:
	struct InputBinding final
	{
		PostProcessingInputType type;
		u32 texture_unit;
		TargetSize size;

		D3DTexture2D* texture;	// only set for external images
		ID3D11ShaderResourceView* texture_srv;
		Microsoft::WRL::ComPtr<ID3D11SamplerState> texture_sampler;
	};

	struct RenderPassData final
	{
		D3D::PixelShaderPtr pixel_shader;

		std::vector<InputBinding> inputs;

		D3DTexture2D* output_texture;
		TargetSize output_size;
		float output_scale;

		bool enabled;
	};

	bool CreatePasses();
	bool RecompileShaders();
	bool ResizeOutputTextures(const TargetSize& new_size);
	void LinkPassOutputs();

	const PostProcessingShaderConfiguration* m_config;

	TargetSize m_internal_size;
	int m_internal_layers = 0;

	std::vector<RenderPassData> m_passes;
	size_t m_last_pass_index = 0;
	bool m_last_pass_uses_color_buffer = false;
	bool m_ready = false;
};

class D3DPostProcessor final : public PostProcessor
{
public:
	D3DPostProcessor() = default;
	~D3DPostProcessor();

	bool Initialize() override;

	void ReloadShaders() override;

	void PostProcessEFB() override;

	void BlitScreen(const TargetRectangle& dst_rect, const TargetSize& dst_size, uintptr_t dst_texture,
		const TargetRectangle& src_rect, const TargetSize& src_size, uintptr_t src_texture, uintptr_t src_depth_texture,
		int src_layer, float gamma) override;

	void PostProcess(TargetRectangle* output_rect, TargetSize* output_size, uintptr_t* output_texture,
		const TargetRectangle& src_rect, const TargetSize& src_size, uintptr_t src_texture,
		const TargetRectangle& src_depth_rect, const TargetSize& src_depth_size, uintptr_t src_depth_texture) override;

	void MapAndUpdateUniformBuffer(const PostProcessingShaderConfiguration* config,
		const InputTextureSizeArray& input_sizes,
		const TargetRectangle& dst_rect, const TargetSize& dst_size,
		const TargetRectangle& src_rect, const TargetSize& src_size,
		int src_layer, float gamma);

	// NOTE: Can change current render target and viewport.
	// If src_layer <0, copy all layers, otherwise, copy src_layer to layer 0.
	static void CopyTexture(const TargetRectangle& dst_rect, D3DTexture2D* dst_texture,
		const TargetRectangle& src_rect, D3DTexture2D* src_texture,
		const TargetSize& src_size, int src_layer,
		bool force_shader_copy = false);

	// Shadered shader stages
	ID3D11VertexShader* GetVertexShader() const { return m_vertex_shader.get(); }
	ID3D11GeometryShader* GetGeometryShader() const { return m_geometry_shader.get(); }

protected:
	bool CreateCommonShaders();
	bool CreateUniformBuffer();

	std::unique_ptr<PostProcessingShader> CreateShader(const PostProcessingShaderConfiguration* config);
	void CreatePostProcessingShaders();
	void CreateScalingShader();
	void CreateStereoShader();

	bool ResizeCopyBuffers(const TargetSize& size, int layers);
	bool ResizeStereoBuffer(const TargetSize& size);
	bool ReconfigurePostProcessingShaders(const TargetSize& size);
	bool ReconfigureScalingShader(const TargetSize& size);
	bool ReconfigureStereoShader(const TargetSize& size);

	void DrawStereoBuffers(const TargetRectangle& dst_rect, const TargetSize& dst_size, D3DTexture2D* dst_texture,
		const TargetRectangle& src_rect, const TargetSize& src_size, D3DTexture2D* src_texture, D3DTexture2D* src_depth_texture, float gamma);

	void DisablePostProcessor();

	D3D::VertexShaderPtr m_vertex_shader;
	D3D::GeometryShaderPtr m_geometry_shader;
	D3D::BufferPtr m_uniform_buffer;

	std::unique_ptr<PostProcessingShader> m_scaling_shader;
	std::unique_ptr<PostProcessingShader> m_stereo_shader;
	std::vector<std::unique_ptr<PostProcessingShader>> m_post_processing_shaders;

	TargetSize m_copy_size;
	int m_copy_layers = 0;
	D3DTexture2D* m_color_copy_texture = nullptr;
	D3DTexture2D* m_depth_copy_texture = nullptr;

	TargetSize m_stereo_buffer_size;
	D3DTexture2D* m_stereo_buffer_texture = nullptr;
};

}  // namespace
