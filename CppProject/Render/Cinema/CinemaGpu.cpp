#include "CinemaGpu.hpp"

#include <cmath>
#include <cstring>

// CinemaGpu implementation (CINEMA_DESIGN.md, sections 2 and 3).
// See CinemaGpu.hpp for the error policy: no failure path may crash.

namespace CppProject
{
#if API_D3D11

	BoolType CinemaGpuSupported()
	{
		if (!GFX || !D3DDevice)
			return false;

		// Same gate pattern as GraphicsApiHandler::GetMaxSize: cs_5_0 is
		// only guaranteed from feature level 11_0 upwards.
		return D3DDevice->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0;
	}

	ID3D11ComputeShader* CinemaGpu::CreateCS(const char* hlslSource, const char* entry, const char* debugName)
	{
		if (!hlslSource || !entry)
		{
			WARNING("Null source or entry point");
			return nullptr;
		}

		if (!CinemaGpuSupported())
		{
			WARNING("Compute shaders require feature level 11_0");
			return nullptr;
		}

		// IEEE strictness: the M0 kernel relies on isfinite() to guarantee a
		// NaN/inf-free accumulator (CinemaKernelM0.hpp compilation contract);
		// without it the optimizer may fold the check away.
		UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS;
	#if DEBUG_MODE
		flags |= D3DCOMPILE_DEBUG;
	#else
		flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
	#endif

		ID3DBlob* data = nullptr;
		ID3DBlob* errMsgs = nullptr;
		HRESULT result = D3DCompile(hlslSource, strlen(hlslSource), debugName,
			nullptr, nullptr, entry, "cs_5_0", flags, 0, &data, &errMsgs);

		if (FAILED(result))
		{
			QString errMsg = "(no compiler log)";
			if (errMsgs)
				errMsg = QString::fromLatin1((const char*)errMsgs->GetBufferPointer(), (int)errMsgs->GetBufferSize());
			WARNING("Compiling compute shader " + QString(debugName) + " failed\n\t" + errMsg);
			releaseAndReset(errMsgs);
			releaseAndReset(data);
			return nullptr;
		}
		releaseAndReset(errMsgs);

		ID3D11ComputeShader* cs = nullptr;
		result = D3DDevice->CreateComputeShader(data->GetBufferPointer(), data->GetBufferSize(), nullptr, &cs);
		releaseAndReset(data);

		if (FAILED(result))
		{
			WARNING("CreateComputeShader failed for " + QString(debugName) + " (HRESULT " + NumStr((IntType)result) + ")");
			return nullptr;
		}

		return cs;
	}

	ID3D11Buffer* CinemaGpu::CreateStructuredBuffer(UINT stride, UINT count, const void* data, BoolType uav)
	{
		if (!stride || !count)
		{
			WARNING("Zero stride or count");
			return nullptr;
		}

		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = stride * count;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (uav)
			desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = stride;

		D3D11_SUBRESOURCE_DATA init = {};
		init.pSysMem = data;

		ID3D11Buffer* buffer = nullptr;
		HRESULT result = D3DDevice->CreateBuffer(&desc, data ? &init : nullptr, &buffer);
		if (FAILED(result))
		{
			WARNING("CreateBuffer failed, stride " + NumStr((IntType)stride) + " count " + NumStr((IntType)count)
				+ " (HRESULT " + NumStr((IntType)result) + ")");
			return nullptr;
		}

		return buffer;
	}

	ID3D11ShaderResourceView* CinemaGpu::CreateBufferSRV(ID3D11Buffer* buffer, UINT count)
	{
		if (!buffer || !count)
		{
			WARNING("Null buffer or zero count");
			return nullptr;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
		desc.Format = DXGI_FORMAT_UNKNOWN; // structured buffers use the element stride
		desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		desc.Buffer.FirstElement = 0;
		desc.Buffer.NumElements = count;

		ID3D11ShaderResourceView* srv = nullptr;
		HRESULT result = D3DDevice->CreateShaderResourceView(buffer, &desc, &srv);
		if (FAILED(result))
		{
			WARNING("CreateShaderResourceView failed (HRESULT " + NumStr((IntType)result) + ")");
			return nullptr;
		}

		return srv;
	}

	ID3D11UnorderedAccessView* CinemaGpu::CreateBufferUAV(ID3D11Buffer* buffer, UINT count)
	{
		if (!buffer || !count)
		{
			WARNING("Null buffer or zero count");
			return nullptr;
		}

		D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {};
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		desc.Buffer.FirstElement = 0;
		desc.Buffer.NumElements = count;

		ID3D11UnorderedAccessView* uav = nullptr;
		HRESULT result = D3DDevice->CreateUnorderedAccessView(buffer, &desc, &uav);
		if (FAILED(result))
		{
			WARNING("CreateUnorderedAccessView failed (HRESULT " + NumStr((IntType)result) + ")");
			return nullptr;
		}

		return uav;
	}

	ID3D11Texture2D* CinemaGpu::CreateTexture2DArraySRGB(UINT w, UINT h, UINT slices, const void* const* sliceData)
	{
		if (!w || !h || !slices || !sliceData)
		{
			WARNING("Invalid size or null slice data");
			return nullptr;
		}

		QVector<D3D11_SUBRESOURCE_DATA> init((int)slices);
		for (UINT s = 0; s < slices; s++)
		{
			if (!sliceData[s])
			{
				WARNING("Null pixel data for slice " + NumStr((IntType)s));
				return nullptr;
			}
			init[(int)s].pSysMem = sliceData[s];
			init[(int)s].SysMemPitch = w * 4;
			init[(int)s].SysMemSlicePitch = w * h * 4;
		}

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = w;
		desc.Height = h;
		desc.MipLevels = 1; // nearest sampling at mip 0 only (Minecraft look, no atlas bleeding)
		desc.ArraySize = slices;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		ID3D11Texture2D* tex = nullptr;
		HRESULT result = D3DDevice->CreateTexture2D(&desc, init.constData(), &tex);
		if (FAILED(result))
		{
			WARNING("CreateTexture2D failed, " + NumStr((IntType)w) + "x" + NumStr((IntType)h)
				+ " x " + NumStr((IntType)slices) + " slices (HRESULT " + NumStr((IntType)result) + ")");
			return nullptr;
		}

		return tex;
	}

	ID3D11Texture2D* CinemaGpu::CreateTexture2DUAV(UINT w, UINT h)
	{
		if (!w || !h)
		{
			WARNING("Zero size");
			return nullptr;
		}

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = w;
		desc.Height = h;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		ID3D11Texture2D* tex = nullptr;
		HRESULT result = D3DDevice->CreateTexture2D(&desc, nullptr, &tex);
		if (FAILED(result))
		{
			WARNING("CreateTexture2D failed, " + NumStr((IntType)w) + "x" + NumStr((IntType)h)
				+ " RGBA32F (HRESULT " + NumStr((IntType)result) + ")");
			return nullptr;
		}

		return tex;
	}

	ID3D11ShaderResourceView* CinemaGpu::CreateTextureSRV(ID3D11Texture2D* tex)
	{
		if (!tex)
		{
			WARNING("Null texture");
			return nullptr;
		}

		// Null desc = view over the full resource (works for 2D and arrays).
		ID3D11ShaderResourceView* srv = nullptr;
		HRESULT result = D3DDevice->CreateShaderResourceView(tex, nullptr, &srv);
		if (FAILED(result))
		{
			WARNING("CreateShaderResourceView failed (HRESULT " + NumStr((IntType)result) + ")");
			return nullptr;
		}

		return srv;
	}

	ID3D11UnorderedAccessView* CinemaGpu::CreateTextureUAV(ID3D11Texture2D* tex)
	{
		if (!tex)
		{
			WARNING("Null texture");
			return nullptr;
		}

		ID3D11UnorderedAccessView* uav = nullptr;
		HRESULT result = D3DDevice->CreateUnorderedAccessView(tex, nullptr, &uav);
		if (FAILED(result))
		{
			WARNING("CreateUnorderedAccessView failed (HRESULT " + NumStr((IntType)result) + ")");
			return nullptr;
		}

		return uav;
	}

	ID3D11Texture2D* CinemaGpu::CreateStagingCopy(ID3D11Texture2D* tex)
	{
		if (!tex)
		{
			WARNING("Null texture");
			return nullptr;
		}

		// Pattern: FrameBuffer::CopyData (staging copy + CopyResource).
		D3D11_TEXTURE2D_DESC desc = {};
		tex->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		desc.MiscFlags = 0;

		ID3D11Texture2D* stagingTex = nullptr;
		HRESULT result = D3DDevice->CreateTexture2D(&desc, nullptr, &stagingTex);
		if (FAILED(result))
		{
			WARNING("Creating staging texture failed (HRESULT " + NumStr((IntType)result) + ")");
			return nullptr;
		}

		D3DContext->CopyResource(stagingTex, tex);
		return stagingTex;
	}

	BoolType CinemaGpu::ReadbackTexture(ID3D11Texture2D* tex, void* dst, size_t dstRowSize, UINT rows)
	{
		if (!tex || !dst || !dstRowSize || !rows)
		{
			WARNING("Invalid readback arguments");
			return false;
		}

		ID3D11Texture2D* stagingTex = CreateStagingCopy(tex);
		if (!stagingTex)
			return false;

		D3D11_TEXTURE2D_DESC desc = {};
		stagingTex->GetDesc(&desc);
		if (rows > desc.Height)
		{
			WARNING("Row count " + NumStr((IntType)rows) + " exceeds texture height " + NumStr((IntType)desc.Height));
			releaseAndReset(stagingTex);
			return false;
		}

		D3D11_MAPPED_SUBRESOURCE texRes = {};
		HRESULT result = D3DContext->Map(stagingTex, 0, D3D11_MAP_READ, 0, &texRes);
		if (FAILED(result))
		{
			WARNING("Map failed (HRESULT " + NumStr((IntType)result) + ")");
			releaseAndReset(stagingTex);
			return false;
		}

		// Copy row by row, respecting RowPitch (may exceed the packed size).
		size_t rowSize = dstRowSize;
		if (rowSize > texRes.RowPitch)
		{
			WARNING("Destination row size " + NumStr((IntType)rowSize) + " exceeds RowPitch "
				+ NumStr((IntType)texRes.RowPitch) + ", clamping");
			rowSize = texRes.RowPitch;
		}
		for (UINT y = 0; y < rows; y++)
			memcpy((uchar*)dst + y * dstRowSize,
				   (uchar*)texRes.pData + y * texRes.RowPitch,
				   rowSize);

		D3DContext->Unmap(stagingTex, 0);
		releaseAndReset(stagingTex);
		return true;
	}

	ID3D11Buffer* CinemaGpu::CreateStagingBufferCopy(ID3D11Buffer* buffer)
	{
		if (!buffer)
		{
			WARNING("Null buffer");
			return nullptr;
		}

		// Buffer flavor of CreateStagingCopy (staging copy + CopyResource).
		D3D11_BUFFER_DESC desc = {};
		buffer->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;

		ID3D11Buffer* stagingBuffer = nullptr;
		HRESULT result = D3DDevice->CreateBuffer(&desc, nullptr, &stagingBuffer);
		if (FAILED(result))
		{
			WARNING("Creating staging buffer failed, " + NumStr((IntType)desc.ByteWidth)
				+ " bytes (HRESULT " + NumStr((IntType)result) + ")");
			return nullptr;
		}

		D3DContext->CopyResource(stagingBuffer, buffer);
		return stagingBuffer;
	}

	BoolType CinemaGpu::ReadbackBuffer(ID3D11Buffer* buffer, void* dst, size_t sizeBytes)
	{
		if (!buffer || !dst || !sizeBytes)
		{
			WARNING("Invalid readback arguments");
			return false;
		}

		ID3D11Buffer* stagingBuffer = CreateStagingBufferCopy(buffer);
		if (!stagingBuffer)
			return false;

		D3D11_BUFFER_DESC desc = {};
		stagingBuffer->GetDesc(&desc);
		if (sizeBytes > desc.ByteWidth)
		{
			WARNING("Read size " + NumStr((IntType)sizeBytes) + " exceeds buffer size "
				+ NumStr((IntType)desc.ByteWidth));
			releaseAndReset(stagingBuffer);
			return false;
		}

		D3D11_MAPPED_SUBRESOURCE bufRes = {};
		HRESULT result = D3DContext->Map(stagingBuffer, 0, D3D11_MAP_READ, 0, &bufRes);
		if (FAILED(result))
		{
			WARNING("Map failed (HRESULT " + NumStr((IntType)result) + ")");
			releaseAndReset(stagingBuffer);
			return false;
		}

		memcpy(dst, bufRes.pData, sizeBytes);

		D3DContext->Unmap(stagingBuffer, 0);
		releaseAndReset(stagingBuffer);
		return true;
	}

	ID3D11Buffer* CinemaGpu::CreateConstantBuffer(UINT sizeBytes, const void* data)
	{
		if (!sizeBytes)
		{
			WARNING("Zero size");
			return nullptr;
		}

		// Constant buffers must be a multiple of 16 bytes.
		UINT paddedSize = (sizeBytes + 15) & ~15u;

		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = paddedSize;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

		// Zero-pad the initial data if the struct is not 16-byte aligned.
		QVector<uchar> padded;
		D3D11_SUBRESOURCE_DATA init = {};
		if (data)
		{
			if (paddedSize != sizeBytes)
			{
				padded.resize((int)paddedSize);
				padded.fill(0);
				memcpy(padded.data(), data, sizeBytes);
				init.pSysMem = padded.constData();
			}
			else
				init.pSysMem = data;
		}

		ID3D11Buffer* cb = nullptr;
		HRESULT result = D3DDevice->CreateBuffer(&desc, data ? &init : nullptr, &cb);
		if (FAILED(result))
		{
			WARNING("CreateBuffer failed, " + NumStr((IntType)paddedSize) + " bytes (HRESULT " + NumStr((IntType)result) + ")");
			return nullptr;
		}

		return cb;
	}

	void CinemaGpu::UpdateConstantBuffer(ID3D11Buffer* cb, const void* data, UINT sizeBytes)
	{
		if (!cb || !data || !sizeBytes)
		{
			WARNING("Null buffer or data");
			return;
		}

		D3D11_BUFFER_DESC desc = {};
		cb->GetDesc(&desc);
		if (sizeBytes > desc.ByteWidth)
		{
			WARNING("Data size " + NumStr((IntType)sizeBytes) + " exceeds buffer size "
				+ NumStr((IntType)desc.ByteWidth));
			return;
		}

		// A full-resource UpdateSubresource reads desc.ByteWidth bytes from
		// the source pointer. When the buffer was padded past the struct
		// size, go through a zero-padded copy so the read never runs past
		// the caller's struct.
		if (sizeBytes == desc.ByteWidth)
			D3DContext->UpdateSubresource(cb, 0, nullptr, data, 0, 0);
		else
		{
			QVector<uchar> padded((int)desc.ByteWidth);
			padded.fill(0);
			memcpy(padded.data(), data, sizeBytes);
			D3DContext->UpdateSubresource(cb, 0, nullptr, padded.constData(), 0, 0);
		}
	}

	void CinemaGpu::Dispatch(ID3D11ComputeShader* cs, ID3D11Buffer* cb,
		ID3D11ShaderResourceView* const* srvs, UINT numSrvs,
		ID3D11UnorderedAccessView* const* uavs, UINT numUavs,
		UINT gx, UINT gy)
	{
		if (!cs || !gx || !gy)
		{
			WARNING("Null shader or zero group count");
			return;
		}

		if (numSrvs > MAX_SRV_SLOTS)
		{
			WARNING("SRV count " + NumStr((IntType)numSrvs) + " exceeds " + NumStr((IntType)MAX_SRV_SLOTS));
			return;
		}

		if (numUavs > MAX_UAV_SLOTS)
		{
			WARNING("UAV count " + NumStr((IntType)numUavs) + " exceeds " + NumStr((IntType)MAX_UAV_SLOTS));
			return;
		}

		// Bind
		D3DContext->CSSetShader(cs, nullptr, 0);
		if (cb)
			D3DContext->CSSetConstantBuffers(0, 1, &cb);
		if (numSrvs && srvs)
			D3DContext->CSSetShaderResources(0, numSrvs, srvs);
		if (numUavs && uavs)
			D3DContext->CSSetUnorderedAccessViews(0, numUavs, uavs, nullptr);

		D3DContext->Dispatch(gx, gy, 1);

		// Clean unbind: leaving CS SRVs/UAVs bound would hazard against the
		// editor raster passes that render to or sample the same resources.
		ID3D11ShaderResourceView* nullSrvs[MAX_SRV_SLOTS] = {};
		ID3D11UnorderedAccessView* nullUavs[MAX_UAV_SLOTS] = {};
		ID3D11Buffer* nullCb = nullptr;

		if (numSrvs)
			D3DContext->CSSetShaderResources(0, numSrvs, nullSrvs);
		if (numUavs)
			D3DContext->CSSetUnorderedAccessViews(0, numUavs, nullUavs, nullptr);
		if (cb)
			D3DContext->CSSetConstantBuffers(0, 1, &nullCb);
		D3DContext->CSSetShader(nullptr, nullptr, 0);
	}

	// ---------------------------------------------------------------------
	// Selftest
	// ---------------------------------------------------------------------

	namespace
	{
		const UINT SELFTEST_SIZE = 64;
		const UINT SELFTEST_SEED = 7;

		// Trivial kernel: writes a deterministic function of the thread id.
		// Integer math keeps the CPU reference bit-comparable; the final
		// divisions are compared with a small epsilon (the GPU may fold a
		// division into a reciprocal multiply).
		const char* SELFTEST_HLSL = R"hlsl(
cbuffer SelftestFrame : register(b0)
{
	uint4 uParams; // x = width, y = height, z = seed, w = unused
};

RWTexture2D<float4> uOutput : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= uParams.x || id.y >= uParams.y)
		return;

	float4 result;
	result.x = float(id.x ^ id.y) / 255.0;
	result.y = float((id.x + 2u * id.y + uParams.z) & 1023u) / 1023.0;
	result.z = float((7u * id.x + 13u * id.y) & 255u) / 255.0;
	result.w = 1.0;
	uOutput[id.xy] = result;
}
)hlsl";

		// CPU reference of the kernel above (same formula).
		void SelftestReference(UINT x, UINT y, UINT seed, float out[4])
		{
			out[0] = (float)(x ^ y) / 255.0f;
			out[1] = (float)((x + 2u * y + seed) & 1023u) / 1023.0f;
			out[2] = (float)((7u * x + 13u * y) & 255u) / 255.0f;
			out[3] = 1.0f;
		}

		struct SelftestParams
		{
			uint32_t width, height, seed, pad;
		};
	}

	BoolType CinemaSelftest(QString* errorOut)
	{
		QString error;
		BoolType ok = false;

		ID3D11ComputeShader* cs = nullptr;
		ID3D11Texture2D* tex = nullptr;
		ID3D11UnorderedAccessView* uav = nullptr;
		ID3D11Buffer* cb = nullptr;

		if (!CinemaGpuSupported())
			error = "cinema_requires_fl11";

		if (error.isEmpty())
		{
			cs = CinemaGpu::CreateCS(SELFTEST_HLSL, "main", "cinema_selftest");
			if (!cs)
				error = "Selftest kernel compilation failed";
		}

		if (error.isEmpty())
		{
			tex = CinemaGpu::CreateTexture2DUAV(SELFTEST_SIZE, SELFTEST_SIZE);
			uav = tex ? CinemaGpu::CreateTextureUAV(tex) : nullptr;
			if (!uav)
				error = "Selftest UAV creation failed";
		}

		if (error.isEmpty())
		{
			SelftestParams params = { SELFTEST_SIZE, SELFTEST_SIZE, SELFTEST_SEED, 0 };
			cb = CinemaGpu::CreateConstantBuffer(sizeof(params), &params);
			if (!cb)
				error = "Selftest constant buffer creation failed";
		}

		if (error.isEmpty())
		{
			CinemaGpu::Dispatch(cs, cb, nullptr, 0, &uav, 1, SELFTEST_SIZE / 8, SELFTEST_SIZE / 8);

			// Readback and verify against the CPU reference
			QVector<float> pixels((int)(SELFTEST_SIZE * SELFTEST_SIZE * 4));
			if (!CinemaGpu::ReadbackTexture(tex, pixels.data(), SELFTEST_SIZE * 4 * sizeof(float), SELFTEST_SIZE))
				error = "Selftest readback failed";
			else
			{
				const float epsilon = 1.0e-5f;
				IntType mismatches = 0;
				for (UINT y = 0; y < SELFTEST_SIZE && mismatches == 0; y++)
				{
					for (UINT x = 0; x < SELFTEST_SIZE && mismatches == 0; x++)
					{
						float expected[4];
						SelftestReference(x, y, SELFTEST_SEED, expected);
						const float* got = pixels.constData() + (y * SELFTEST_SIZE + x) * 4;

						for (IntType c = 0; c < 4; c++)
						{
							// NaN-safe: any comparison with NaN is false
							if (!(fabsf(got[c] - expected[c]) <= epsilon))
							{
								error = "Selftest mismatch at " + NumStr((IntType)x) + "," + NumStr((IntType)y)
									+ " channel " + NumStr(c) + ": expected " + NumStr(expected[c])
									+ ", got " + NumStr(got[c]);
								mismatches++;
								break;
							}
						}
					}
				}

				if (!mismatches)
					ok = true;
			}
		}

		releaseAndReset(cb);
		releaseAndReset(uav);
		releaseAndReset(tex);
		releaseAndReset(cs);

		if (ok)
			DEBUG("Cinema GPU selftest passed");
		else
			WARNING("Cinema GPU selftest failed: " + error);

		if (errorOut)
			*errorOut = ok ? QString() : error;
		return ok;
	}

#else // !API_D3D11

	BoolType CinemaGpuSupported()
	{
		return false;
	}

	BoolType CinemaSelftest(QString* errorOut)
	{
		if (errorOut)
			*errorOut = "cinema_requires_d3d11";
		return false;
	}

#endif
}
