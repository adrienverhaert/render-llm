#include "Texture.hpp"

#include "GraphicsApiHandler.hpp"

namespace CppProject
{
	QHash<IntType, BoolType> Texture::hasMipMaps;

#if API_D3D11
	QHash<IntType, ID3D11ShaderResourceView*> Texture::d3dIdSRVMap;
	IntType Texture::d3dSRVNextId = 1;

	Texture::Texture(const QImage& img)
	{
		if (!img.width() || !img.height())
			return;

		// Create texture
		const uchar* imgBits = img.constBits();
		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = img.width();
		texDesc.Height = img.height();
		texDesc.MipLevels = 0;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		texDesc.CPUAccessFlags = 0;
		texDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

		// DirectX 10 max width/height
		QImage imgScaled;
		IntType maxSize = GFX->GetMaxSize();
		if (img.width() > maxSize || img.height() > maxSize)
		{
			if (img.width() > maxSize) 
				imgScaled = img.scaledToWidth(maxSize);
			else
				imgScaled = img.scaledToHeight(maxSize);

			texDesc.Width = imgScaled.width();
			texDesc.Height = imgScaled.height();
			imgBits = imgScaled.constBits();
		}

		D3DCheckError(D3DDevice->CreateTexture2D(&texDesc, nullptr, &d3dTex));

		// Create SRV
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = -1;
		D3DCheckError(D3DDevice->CreateShaderResourceView(d3dTex, &srvDesc, &d3dSRV));
		d3dIdSRVMap[d3dSRVNextId] = d3dSRV;
		d3dSRVId = d3dSRVNextId++;

		// Upload data from QImage
		D3DContext->UpdateSubresource(d3dTex, 0, nullptr, imgBits, texDesc.Width * 4, texDesc.Width * texDesc.Height * 4);

		// MI3: initialize the FULL mip chain right away. MipLevels=0 allocates
		// every level but UpdateSubresource only fills level 0 — the rest stay
		// uninitialized (black). The lazy GenerateMips in GetId() only runs
		// when GFX->mipMap happens to be on at bind time, so a texture first
		// used while mips were off keeps garbage levels forever and renders
		// INVISIBLE the moment any mip-filtered sampler touches it (runtime-
		// created textures: custom armor, generated armor skins, dyes, trims).
		D3DContext->GenerateMips(d3dSRV);
		hasMipMaps[d3dSRVId] = true;
	}

	Texture::~Texture()
	{
		if (d3dSRVId)
		{
			d3dIdSRVMap.remove(d3dSRVId);
			releaseAndReset(d3dTex);
			releaseAndReset(d3dSRV);

			hasMipMaps.remove(d3dSRVId);
		}
	}

	IntType Texture::GetId()
	{
		// Generate mipmaps if needed
		if (d3dSRVId && GFX->mipMap && !hasMipMaps.contains(d3dSRVId))
		{
			D3DContext->GenerateMips(d3dSRV);
			hasMipMaps[d3dSRVId] = true;
		}
		return d3dSRVId;
	}
#else
	Texture::Texture(const QImage& img)
	{
		// Flip incoming data vertically
		IntType rowSize = img.width() * 4;
		GLubyte* data = new GLubyte[rowSize * img.height()];
		for (IntType y = 0; y < img.height(); y++)
			memcpy( data + rowSize * y, img.scanLine(img.height() - 1 - y), rowSize);

		GFX->glGenTextures(1, &glTexId);
		GFX->glBindTexture(GL_TEXTURE_2D, glTexId);
		GFX->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width(), img.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
		// MI3: complete the mip chain at creation — a mip-filtered sampler on a
		// mip-incomplete GL texture samples BLACK (same landmine as the D3D11
		// path, where the allocated-but-unfilled levels read as garbage).
		GFX->glGenerateMipmap(GL_TEXTURE_2D);
		GFX->glBindTexture(GL_TEXTURE_2D, 0);
		GL_CHECK_ERROR();

		hasMipMaps[glTexId] = true;

		delete[] data;
	}

	Texture::~Texture()
	{
		GFX->glDeleteTextures(1, &glTexId);
		GL_CHECK_ERROR();

		hasMipMaps.remove(glTexId);
	}

	IntType Texture::GetId()
	{
		// Generate mipmaps if needed
		if (GFX->mipMap && !hasMipMaps.contains(glTexId))
		{
			GFX->glBindTexture(GL_TEXTURE_2D, glTexId);
			GFX->glGenerateMipmap(GL_TEXTURE_2D);
			GFX->glBindTexture(GL_TEXTURE_2D, 0);
			GL_CHECK_ERROR();

			hasMipMaps[glTexId] = true;
		}

		return glTexId;
	}
#endif
}