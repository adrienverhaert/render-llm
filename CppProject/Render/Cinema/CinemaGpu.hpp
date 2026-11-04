#pragma once

// CinemaGpu: minimal D3D11 compute helpers for the Cinema path tracer
// (CINEMA_DESIGN.md, sections 2 and 3). No compute/UAV/structured buffer
// code exists elsewhere in the repo; these helpers wrap the existing
// D3DDevice/D3DContext macros from GraphicsApiHandler.hpp.
//
// Error policy: never crash. Every failure releases what it allocated,
// emits a WARNING and returns nullptr/false. HRESULTs are checked
// directly (not via D3DCheckError, which may throw on device errors).

#include "Common.hpp"
#include "Render/GraphicsApiHandler.hpp"

namespace CppProject
{
	// Returns whether the Cinema engine can run on this GPU: feature level
	// 11_0 or higher is required for cs_5_0 compute shaders (same gate
	// pattern as GraphicsApiHandler::GetMaxSize). Always false on OpenGL.
	BoolType CinemaGpuSupported();

	// Compiles a trivial embedded compute kernel, writes a deterministic
	// pattern to a 64x64 RGBA32F UAV, reads it back and verifies it on the
	// CPU. Returns true on success; on failure returns false and stores a
	// human-readable message in errorOut (if given). Never crashes.
	BoolType CinemaSelftest(QString* errorOut = nullptr);

#if API_D3D11
	// Static helper collection (pattern: Printer, Random in Common.hpp).
	struct CinemaGpu
	{
		// Maximum bind slots accepted by Dispatch below. 8 UAVs is the
		// guaranteed cs_5_0 limit at feature level 11_0.
		static const UINT MAX_SRV_SLOTS = 32;
		static const UINT MAX_UAV_SLOTS = 8;

		// Compiles embedded HLSL source text as a cs_5_0 compute shader.
		// The compiler log is forwarded to WARNING on failure and the
		// function returns nullptr. debugName only tags log messages.
		static ID3D11ComputeShader* CreateCS(const char* hlslSource, const char* entry, const char* debugName = "cinema_cs");

		// Creates a structured buffer of count elements of stride bytes.
		// data may be nullptr (uninitialized, GPU-writable use). When uav
		// is true the buffer may also be bound through a UAV.
		static ID3D11Buffer* CreateStructuredBuffer(UINT stride, UINT count, const void* data, BoolType uav);

		// Views over a structured buffer created above.
		static ID3D11ShaderResourceView* CreateBufferSRV(ID3D11Buffer* buffer, UINT count);
		static ID3D11UnorderedAccessView* CreateBufferUAV(ID3D11Buffer* buffer, UINT count);

		// Creates an immutable RGBA8 sRGB texture array (the texture atlas;
		// the SRGB format gives sRGB -> linear conversion for free on
		// sample). sliceData holds one pointer per slice to w*h*4 bytes of
		// RGBA8 pixels; no slice pointer may be null.
		static ID3D11Texture2D* CreateTexture2DArraySRGB(UINT w, UINT h, UINT slices, const void* const* sliceData);

		// Creates an RGBA32F texture bindable as both UAV and SRV
		// (accumulators and AOVs).
		static ID3D11Texture2D* CreateTexture2DUAV(UINT w, UINT h);

		// Full-resource views over textures created by the helpers above.
		static ID3D11ShaderResourceView* CreateTextureSRV(ID3D11Texture2D* tex);
		static ID3D11UnorderedAccessView* CreateTextureUAV(ID3D11Texture2D* tex);

		// Creates a CPU-readable staging copy of tex and copies the GPU
		// contents into it (pattern: FrameBuffer::CopyData). Caller owns
		// the returned texture and Maps/Unmaps it, or uses ReadbackTexture.
		static ID3D11Texture2D* CreateStagingCopy(ID3D11Texture2D* tex);

		// Copies the full contents of tex into dst. dstRowSize is the
		// tightly-packed destination row size in bytes (width * bytes per
		// pixel); rows must match the texture height. Respects RowPitch.
		static BoolType ReadbackTexture(ID3D11Texture2D* tex, void* dst, size_t dstRowSize, UINT rows);

		// Buffer flavor of CreateStagingCopy: creates a CPU-readable staging
		// copy of buffer and copies the GPU contents into it. Caller owns the
		// returned buffer and Maps/Unmaps it, or uses ReadbackBuffer.
		static ID3D11Buffer* CreateStagingBufferCopy(ID3D11Buffer* buffer);

		// Copies the first sizeBytes bytes of the buffer contents into dst
		// (staging copy + Map; buffers have no RowPitch, plain memcpy).
		static BoolType ReadbackBuffer(ID3D11Buffer* buffer, void* dst, size_t sizeBytes);

		// Creates a constant buffer of sizeBytes (padded up to a multiple
		// of 16 internally). data may be nullptr.
		static ID3D11Buffer* CreateConstantBuffer(UINT sizeBytes, const void* data);

		// Uploads sizeBytes bytes (the full struct originally passed to
		// CreateConstantBuffer) via UpdateSubresource. If the buffer was
		// padded to a multiple of 16, the tail is zero-filled internally
		// (a full-resource UpdateSubresource reads the whole buffer size
		// from the source pointer, so passing the struct alone would read
		// out of bounds).
		static void UpdateConstantBuffer(ID3D11Buffer* cb, const void* data, UINT sizeBytes);

		// Binds shader + constant buffer + SRVs + UAVs starting at slot 0,
		// dispatches gx * gy * 1 thread groups, then unbinds everything it
		// bound (avoids read/write hazards with the editor raster passes).
		// cb, srvs and uavs may be null/zero when unused.
		static void Dispatch(ID3D11ComputeShader* cs, ID3D11Buffer* cb,
			ID3D11ShaderResourceView* const* srvs, UINT numSrvs,
			ID3D11UnorderedAccessView* const* uavs, UINT numUavs,
			UINT gx, UINT gy);
	};
#endif
}
