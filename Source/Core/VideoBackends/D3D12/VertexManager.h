// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "VideoCommon/VertexManagerBase.h"

namespace DX12
{

class D3DStreamBuffer;

class VertexManager final : public VertexManagerBase
{
public:
	VertexManager();
	~VertexManager();

	NativeVertexFormat* CreateNativeVertexFormat(const PortableVertexDeclaration &_vtx_decl) override;
	void CreateDeviceObjects() override;
	void DestroyDeviceObjects() override;
	void PrepareShaders(PrimitiveType primitive,
		u32 components,
		const XFMemory &xfr,
		const BPMemory &bpm,
		bool fromgputhread = true);
	void SetIndexBuffer();

protected:
	void ResetBuffer(u32 stride) override;
	u16* GetIndexBuffer() override;
private:

	void PrepareDrawBuffers(u32 stride);
	void Draw(u32 stride);
	// temp
	void vFlush(bool use_dst_alpha) override;

	u32 m_vertex_draw_offset;
	u32 m_index_draw_offset;

	D3DStreamBuffer* m_vertex_stream_buffer = nullptr;
	D3DStreamBuffer* m_index_stream_buffer = nullptr;

	bool m_vertex_stream_buffer_reallocated = false;
	bool m_index_stream_buffer_reallocated = false;

	u8* m_index_cpu_buffer = nullptr;
	u8* m_vertex_cpu_buffer = nullptr;

};



}  // namespace
