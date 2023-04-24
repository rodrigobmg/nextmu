#include "stdafx.h"
#include "mu_terrain.h"
#include "mu_graphics.h"
#include "mu_textures.h"
#include "mu_resourcesmanager.h"
#include "mu_state.h"
#include "mu_renderstate.h"
#include "mu_crypt.h"
#include "shared_binaryreader.h"
#include <glm/gtx/normal.hpp>
#include <glm/gtc/packing.hpp>
#include <MapHelper.hpp>

NTerrainVertex TerrainVertices[4 * TerrainSize * TerrainSize] = {};

void InitializeTerrainVertices()
{
	static mu_boolean initialized = false;
	if (initialized) return;
	initialized = true;

	mu_uint32 vertex = 0;
	for (mu_uint32 y = 0; y < TerrainSize; ++y)
	{
		for (mu_uint32 x = 0; x < TerrainSize; ++x)
		{
			NTerrainVertex *vert = &TerrainVertices[vertex++];
			vert->x = 0;
			vert->y = 0;
			vert->rx = x;
			vert->ry = y;

			vert = &TerrainVertices[vertex++];
			vert->x = 1;
			vert->y = 0;
			vert->rx = x;
			vert->ry = y;

			vert = &TerrainVertices[vertex++];
			vert->x = 1;
			vert->y = 1;
			vert->rx = x;
			vert->ry = y;

			vert = &TerrainVertices[vertex++];
			vert->x = 0;
			vert->y = 1;
			vert->rx = x;
			vert->ry = y;
		}
	}
}

constexpr mu_uint32 NumTerrainIndexes = (TerrainSize - 1) * (TerrainSize - 1) * 6;
mu_uint32 TerrainIndexes[NumTerrainIndexes] = {};

void InitializeTerrainIndexes()
{
	static mu_boolean initialized = false;
	if (initialized) return;
	initialized = true;

	mu_uint32 index = 0, vertex = 0;
	for (mu_uint32 y = 0; y < (TerrainSize - 1); ++y)
	{
		for (mu_uint32 x = 0; x < (TerrainSize - 1); ++x)
		{
			TerrainIndexes[index + 0] = vertex + 0;
			TerrainIndexes[index + 1] = vertex + 1;
			TerrainIndexes[index + 2] = vertex + 2;
			TerrainIndexes[index + 3] = vertex + 0;
			TerrainIndexes[index + 4] = vertex + 2;
			TerrainIndexes[index + 5] = vertex + 3;

			index += 6;
			vertex += 4;
		}

		vertex += 4; // Since we are only calculating one less
	}

	index = 0;
}

NTerrain::~NTerrain()
{
	Destroy();
}

void NTerrain::Destroy()
{
#define RELEASE_HANDLER(handler) handler.Release();

	ReleaseShaderResources(TerrainBinding); TerrainBinding = nullptr;
	ReleaseShaderResources(GrassBinding); GrassBinding = nullptr;
	RELEASE_HANDLER(HeightmapTexture);
	RELEASE_HANDLER(LightmapTexture);
	RELEASE_HANDLER(NormalTexture);
	RELEASE_HANDLER(MappingTexture);
	RELEASE_HANDLER(AttributesTexture);
	RELEASE_HANDLER(Textures);
	RELEASE_HANDLER(GrassTextures);
	RELEASE_HANDLER(UVTexture);
	RELEASE_HANDLER(GrassUVTexture);
	RELEASE_HANDLER(VertexBuffer);
	RELEASE_HANDLER(IndexBuffer);
	RELEASE_HANDLER(SettingsUniform);
}

const mu_boolean NTerrain::LoadHeightmap(mu_utf8string path)
{
	NormalizePath(path);

	auto ext = path.substr(path.find_last_of('.') + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), mu_utf8tolower);

	SDL_RWops *fp = nullptr;
	if (mu_rwfromfile<EGameDirectoryType::eSupport>(&fp, path, "rb") == false)
	{
		mu_error("heightmap not found ({})", path);
		return false;
	}

	mu_isize fileLength = static_cast<mu_isize>(SDL_RWsize(fp));

	if (ext == "ozb")
	{
		fileLength -= 4 + 1080;
		SDL_RWseek(fp, 4 + 1080, RW_SEEK_CUR);
	}

	std::unique_ptr<mu_uint8[]> buffer(new_nothrow mu_uint8[fileLength]);
	SDL_RWread(fp, buffer.get(), fileLength, 1);
	SDL_RWclose(fp);

	/*
		OZB file won't be loaded by free image, the image result will be moved by a few pixels,
		instead we load it directly form the buffer.
	*/
	if (ext != "ozb")
	{
		FIMEMORY *memory = FreeImage_OpenMemory(buffer.get(), static_cast<DWORD>(fileLength));
		if (memory == nullptr)
		{
			return false;
		}

		FIBITMAP *bitmap = FreeImage_LoadFromMemory(FREE_IMAGE_FORMAT::FIF_BMP, memory, PNG_IGNOREGAMMA);
		buffer.reset();
		FreeImage_CloseMemory(memory);

		if (bitmap == nullptr)
		{
			return false;
		}

		const FREE_IMAGE_TYPE imageType = FreeImage_GetImageType(bitmap);

		// Only 8bits textures are supported
		if (imageType != FIT_BITMAP)
		{
			FreeImage_Unload(bitmap);
			return false;
		}

		const mu_uint32 width = FreeImage_GetWidth(bitmap);
		const mu_uint32 height = FreeImage_GetHeight(bitmap);
		if (width != TerrainSize || height != TerrainSize)
		{
			FreeImage_Unload(bitmap);
			return false;
		}

		const mu_uint32 pitch = FreeImage_GetPitch(bitmap);
		if (pitch != TerrainSize) // Pitch should be same as TerrainSize since it is a grayscale texture
		{
			FreeImage_Unload(bitmap);
			return false;
		}

		const mu_uint32 bpp = FreeImage_GetBPP(bitmap);
		if (bpp != 8) // Only 8 bits per pixel is supported
		{
			FreeImage_Unload(bitmap);
			return false;
		}

		const mu_uint8 *bitmapBuffer = FreeImage_GetBits(bitmap);
		buffer.reset(new_nothrow mu_uint8[TerrainSize * TerrainSize]);
		mu_memcpy(buffer.get(), bitmapBuffer, TerrainSize * TerrainSize);

		FreeImage_Unload(bitmap);
	}

	if (!TerrainHeight) TerrainHeight.reset(new_nothrow mu_float[TerrainSize * TerrainSize]);
	for (mu_uint32 index = 0; index < TerrainSize * TerrainSize; ++index)
	{
		TerrainHeight[index] = static_cast<mu_float>(buffer[index]) * HeightMultiplier;
	}

	const auto device = MUGraphics::GetDevice();

	std::vector<Diligent::TextureSubResData> subresources;
	Diligent::TextureSubResData subresource;
	subresource.pData = buffer.get();
	subresource.Stride = TerrainSize;
	subresources.push_back(subresource);

	Diligent::TextureDesc textureDesc;
	textureDesc.Type = Diligent::RESOURCE_DIM_TEX_2D;
	textureDesc.Width = TerrainSize;
	textureDesc.Height = TerrainSize;
	textureDesc.Format = Diligent::TEX_FORMAT_R8_UINT;
	textureDesc.Usage = Diligent::USAGE_IMMUTABLE;
	textureDesc.BindFlags = Diligent::BIND_SHADER_RESOURCE;

	Diligent::TextureData textureData(subresources.data(), static_cast<mu_uint32>(subresources.size()));
	Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
	device->CreateTexture(textureDesc, &textureData, &texture);
	if (texture == nullptr)
	{
		return false;
	}

	HeightmapTexture = texture;

	return true;
}

template<typename T, glm::qualifier Q>
GLM_FUNC_QUALIFIER glm::vec<3, T, Q> triangleNormalMU
(
	glm::vec<3, T, Q> const &p1,
	glm::vec<3, T, Q> const &p2,
	glm::vec<3, T, Q> const &p3
)
{
	return normalize(cross(p2 - p1, p3 - p1));
}

const mu_boolean NTerrain::GenerateNormal()
{
	if (!TerrainNormal) TerrainNormal.reset(new_nothrow glm::vec3[TerrainSize * TerrainSize]);
	glm::vec3 *dest = TerrainNormal.get();
	for (mu_uint32 y = 0; y < TerrainSize; ++y)
	{
		for (mu_uint32 x = 0; x < TerrainSize; ++x, ++dest)
		{
			*dest = triangleNormalMU(
				glm::vec3((x + 1) * TerrainScale, y * TerrainScale, GetHeight(x + 1, y)),
				glm::vec3((x + 1) * TerrainScale, (y + 1) * TerrainScale, GetHeight(x + 1, y + 1)),
				glm::vec3(x * TerrainScale, (y + 1) * TerrainScale, GetHeight(x, y + 1))
			);
		}
	}

	if (!NormalMemory) NormalMemory.reset(new_nothrow mu_uint8[sizeof(mu_uint16) * 4 * TerrainSize * TerrainSize]);
	mu_uint64 *data = reinterpret_cast<mu_uint64 *>(NormalMemory.get());
	glm::vec3 *src = TerrainNormal.get();
	for (mu_uint32 y = 0; y < TerrainSize; ++y)
	{
		for (mu_uint32 x = 0; x < TerrainSize; ++x, ++src, ++data)
		{
			*data = glm::packUnorm4x16(glm::vec4(*src, 0.0f));
		}
	}

	const auto device = MUGraphics::GetDevice();

	std::vector<Diligent::TextureSubResData> subresources;
	Diligent::TextureSubResData subresource;
	subresource.pData = NormalMemory.get();
	subresource.Stride = TerrainSize * sizeof(mu_uint16) * 4;
	subresources.push_back(subresource);

	Diligent::TextureDesc textureDesc;
	textureDesc.Type = Diligent::RESOURCE_DIM_TEX_2D;
	textureDesc.Width = TerrainSize;
	textureDesc.Height = TerrainSize;
	textureDesc.Format = Diligent::TEX_FORMAT_RGBA16_UNORM;
	textureDesc.Usage = Diligent::USAGE_DEFAULT;
	textureDesc.BindFlags = Diligent::BIND_SHADER_RESOURCE;

	Diligent::TextureData textureData(subresources.data(), static_cast<mu_uint32>(subresources.size()));
	Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
	device->CreateTexture(textureDesc, &textureData, &texture);
	if (texture == nullptr)
	{
		return false;
	}

	NormalTexture = texture;

	return true;
}

const mu_boolean NTerrain::LoadLightmap(mu_utf8string path)
{
	NormalizePath(path);

	auto ext = path.substr(path.find_last_of('.') + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), mu_utf8tolower);

	SDL_RWops *fp = nullptr;
	if (mu_rwfromfile<EGameDirectoryType::eSupport>(&fp, path, "rb") == false)
	{
		mu_error("heightmap not found ({})", path);
		return false;
	}

	mu_isize fileLength = static_cast<mu_isize>(SDL_RWsize(fp));

	if (ext == "ozj")
	{
		fileLength -= 24;
		SDL_RWseek(fp, 24, RW_SEEK_CUR);
	}

	std::unique_ptr<mu_uint8[]> buffer(new_nothrow mu_uint8[fileLength]);
	SDL_RWread(fp, buffer.get(), fileLength, 1);
	SDL_RWclose(fp);

	FIMEMORY *memory = FreeImage_OpenMemory(buffer.get(), static_cast<DWORD>(fileLength));
	if (memory == nullptr)
	{
		return false;
	}

	FIBITMAP *bitmap = FreeImage_LoadFromMemory(FREE_IMAGE_FORMAT::FIF_JPEG, memory, PNG_IGNOREGAMMA);
	buffer.reset();
	FreeImage_CloseMemory(memory);

	if (bitmap == nullptr)
	{
		return false;
	}

	const FREE_IMAGE_TYPE imageType = FreeImage_GetImageType(bitmap);
	if (imageType != FIT_BITMAP)
	{
		FreeImage_Unload(bitmap);
		return false;
	}

	const mu_uint32 width = FreeImage_GetWidth(bitmap);
	const mu_uint32 height = FreeImage_GetHeight(bitmap);
	if (width != TerrainSize || height != TerrainSize)
	{
		FreeImage_Unload(bitmap);
		return false;
	}

	const mu_uint32 pitch = FreeImage_GetPitch(bitmap);
	const mu_uint32 bpp = FreeImage_GetBPP(bitmap);
	if (pitch != TerrainSize * 4 || bpp != 32)
	{
		FIBITMAP *newBitmap = FreeImage_ConvertTo32Bits(bitmap);
		FreeImage_Unload(bitmap);

		if (newBitmap == nullptr)
		{
			return false;
		}

		bitmap = newBitmap;
	}

	const mu_uint8 *bitmapBuffer = FreeImage_GetBits(bitmap);
	if (!LightmapMemory) LightmapMemory.reset(new_nothrow mu_uint8[sizeof(mu_uint8) * 4 * TerrainSize * TerrainSize]);
	mu_memcpy(LightmapMemory.get(), bitmapBuffer, sizeof(mu_uint8) * 4 * TerrainSize * TerrainSize);
	FreeImage_Unload(bitmap);

	if (!TerrainLight) TerrainLight.reset(new_nothrow glm::vec3[TerrainSize * TerrainSize]);
	if (!TerrainPrimaryLight) TerrainPrimaryLight.reset(new_nothrow glm::vec3[TerrainSize * TerrainSize]);
	const mu_uint32 *lightBuffer = reinterpret_cast<const mu_uint32 *>(LightmapMemory.get());
	const glm::vec3 *normalBuffer = TerrainNormal.get();
	for (mu_uint32 index = 0; index < TerrainSize * TerrainSize; ++index, ++lightBuffer, ++normalBuffer)
	{
		glm::vec3 light(glm::unpackUnorm4x8(*lightBuffer));
		TerrainLight[index] = light * glm::clamp(glm::dot(*normalBuffer, Light) + 0.5f, 0.0f, 1.0f);
	}

	const auto device = MUGraphics::GetDevice();

	std::vector<Diligent::TextureSubResData> subresources;
	Diligent::TextureSubResData subresource;
	subresource.pData = LightmapMemory.get();
	subresource.Stride = TerrainSize * sizeof(mu_uint8) * 4;
	subresources.push_back(subresource);

	Diligent::TextureDesc textureDesc;
	textureDesc.Type = Diligent::RESOURCE_DIM_TEX_2D;
	textureDesc.Width = TerrainSize;
	textureDesc.Height = TerrainSize;
	textureDesc.Format = Diligent::TEX_FORMAT_RGBA8_UNORM;
	textureDesc.Usage = Diligent::USAGE_DEFAULT;
	textureDesc.BindFlags = Diligent::BIND_SHADER_RESOURCE;

	Diligent::TextureData textureData(subresources.data(), static_cast<mu_uint32>(subresources.size()));
	Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
	device->CreateTexture(textureDesc, &textureData, &texture);
	if (texture == nullptr)
	{
		return false;
	}

	LightmapTexture = texture;

	return true;
}

NEXTMU_INLINE mu_uint32 GetPowerOfTwoSize(const mu_uint32 size)
{
	mu_uint32 s = 1;
	while (s < size) s <<= 1;
	return s;
}

struct bitmap_delete { // default deleter for unique_ptr
	constexpr bitmap_delete() noexcept = default;

	constexpr void operator()(FIBITMAP *_Ptr) const noexcept /* strengthened */ { // delete a pointer
		if (_Ptr == nullptr) return;
		FreeImage_Unload(_Ptr);
	}
};

typedef std::unique_ptr<FIBITMAP, bitmap_delete> UniqueBitmap;

const mu_boolean NTerrain::LoadTextures(
	const mu_utf8string dir,
	const nlohmann::json textures,
	const mu_utf8string filter,
	const mu_utf8string wrap,
	const mu_float uvNormal,
	const mu_float uvScaled,
	std::map<mu_uint32, mu_uint32> &texturesMap
)
{
	typedef glm::vec4 SettingFormat;
	mu_uint32 width = 0, height = 0;
	std::vector<SettingFormat> settings;
	std::vector<UniqueBitmap> bitmaps;

	for (auto iter = textures.begin(); iter != textures.end(); ++iter)
	{
		const auto &texture = *iter;
		if (texture.is_object() == false)
		{
			return false;
		}

		const auto id = texture["id"].get<mu_uint32>();
		const auto path = texture["path"].get<mu_utf8string>();

		TextureInfo info;
		FIBITMAP *bitmap = nullptr;
		if (MUTextures::LoadRaw(dir + path, &bitmap, info) == false)
		{
			return false;
		}

		mu_uint32 w = FreeImage_GetWidth(bitmap), h = FreeImage_GetHeight(bitmap);
		if (w > width) width = w;
		if (h > height) height = h;

		const auto scaled = texture["scaled"].get<mu_boolean>();
		const auto water = texture["water"].get<mu_boolean>();

		if (!scaled)
		{
			settings.push_back(SettingFormat(uvNormal / static_cast<mu_float>(w), uvNormal / static_cast<mu_float>(h), static_cast<mu_float>(water), 0.0f));
		}
		else
		{
			settings.push_back(SettingFormat(uvScaled / static_cast<mu_float>(w), uvScaled / static_cast<mu_float>(h), static_cast<mu_float>(water), 0.0f));
		}

		texturesMap.insert(std::pair(id, static_cast<mu_uint32>(bitmaps.size())));
		bitmaps.push_back(UniqueBitmap(bitmap));
	}

	width = GetPowerOfTwoSize(width);
	height = GetPowerOfTwoSize(height);

	for (auto iter = bitmaps.begin(); iter != bitmaps.end(); ++iter)
	{
		auto &ubitmap = *iter;
		FIBITMAP *bitmap = ubitmap.get();

		mu_uint32 w = FreeImage_GetWidth(bitmap), h = FreeImage_GetHeight(bitmap);
		if (w < width || h < height)
		{
			FIBITMAP *newBitmap = FreeImage_Rescale(bitmap, width, height, FREE_IMAGE_FILTER::FILTER_BICUBIC);
			if (newBitmap == nullptr)
			{
				return false;
			}

			ubitmap.reset(newBitmap);
			bitmap = newBitmap;
		}
	}

	const mu_uint16 numLayers = static_cast<mu_uint16>(bitmaps.size());
	const auto device = MUGraphics::GetDevice();

	// Textures
	{
		std::vector<Diligent::TextureSubResData> subresources;
		for (auto iter = bitmaps.begin(); iter != bitmaps.end(); ++iter)
		{
			auto &bitmap = *iter;
			Diligent::TextureSubResData subresource;
			subresource.pData = FreeImage_GetBits(bitmap.get());
			subresource.Stride = width * sizeof(mu_uint8) * 4;
			subresources.push_back(subresource);
		}

		Diligent::TextureDesc textureDesc;
		textureDesc.Type = Diligent::RESOURCE_DIM_TEX_2D_ARRAY;
		textureDesc.Width = width;
		textureDesc.Height = height;
		textureDesc.ArraySize = numLayers;
		textureDesc.Format = Diligent::TEX_FORMAT_RGBA8_UNORM;
		textureDesc.Usage = Diligent::USAGE_IMMUTABLE;
		textureDesc.BindFlags = Diligent::BIND_SHADER_RESOURCE;

		Diligent::TextureData textureData(subresources.data(), static_cast<mu_uint32>(subresources.size()));
		Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
		device->CreateTexture(textureDesc, &textureData, &texture);
		if (texture == nullptr)
		{
			return false;
		}

		texture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE)->SetSampler(GetTextureSampler(MUTextures::CalculateSamplerFlags(filter, wrap)));
		Textures = texture;
	}

	// UV Textures
	{
		const mu_uint32 settingsWidth = GetPowerOfTwoSize(static_cast<mu_uint32>(settings.size()));
		const mu_size memorySize = settingsWidth * sizeof(SettingFormat);
		std::unique_ptr<mu_uint8[]> memory(new (std::nothrow) mu_uint8[memorySize]);
		mu_zeromem(memory.get(), memorySize);
		mu_memcpy(memory.get(), settings.data(), settings.size() * sizeof(SettingFormat));

		std::vector<Diligent::TextureSubResData> subresources;
		Diligent::TextureSubResData subresource;
		subresource.pData = memory.get();
		subresource.Stride = settingsWidth * sizeof(SettingFormat);
		subresources.push_back(subresource);

		Diligent::TextureDesc textureDesc;
		textureDesc.Type = Diligent::RESOURCE_DIM_TEX_2D;
		textureDesc.Width = settingsWidth;
		textureDesc.Height = 1;
		textureDesc.ArraySize = 1;
		textureDesc.Format = Diligent::TEX_FORMAT_RGBA32_FLOAT;
		textureDesc.Usage = Diligent::USAGE_IMMUTABLE;
		textureDesc.BindFlags = Diligent::BIND_SHADER_RESOURCE;

		Diligent::TextureData textureData(subresources.data(), static_cast<mu_uint32>(subresources.size()));
		Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
		device->CreateTexture(textureDesc, &textureData, &texture);
		if (texture == nullptr)
		{
			return false;
		}

		UVTexture = texture;
	}

	return true;
}

const mu_boolean NTerrain::LoadGrassTextures(
	const mu_utf8string dir,
	const nlohmann::json textures,
	const mu_utf8string filter,
	const mu_utf8string wrap,
	std::map<mu_uint32, mu_uint32> &texturesMap
)
{
	typedef mu_float SettingFormat;
	mu_uint32 width = 0, height = 0;
	std::vector<SettingFormat> settings;
	std::vector<UniqueBitmap> bitmaps;

	for (auto iter = textures.begin(); iter != textures.end(); ++iter)
	{
		const auto &texture = *iter;
		if (texture.is_object() == false)
		{
			return false;
		}

		const auto id = texture["id"].get<mu_uint32>();
		const auto path = texture["path"].get<mu_utf8string>();

		TextureInfo info;
		FIBITMAP *bitmap = nullptr;
		if (MUTextures::LoadRaw(dir + path, &bitmap, info) == false)
		{
			return false;
		}

		mu_uint32 w = FreeImage_GetWidth(bitmap), h = FreeImage_GetHeight(bitmap);
		if (w > width) width = w;
		if (h > height) height = h;

		settings.push_back(h * 2.0f);
		texturesMap.insert(std::pair(id, static_cast<mu_uint32>(bitmaps.size())));
		bitmaps.push_back(UniqueBitmap(bitmap));
	}

	width = GetPowerOfTwoSize(width);
	height = GetPowerOfTwoSize(height);

	for (auto iter = bitmaps.begin(); iter != bitmaps.end(); ++iter)
	{
		auto &ubitmap = *iter;
		FIBITMAP *bitmap = ubitmap.get();

		mu_uint32 w = FreeImage_GetWidth(bitmap), h = FreeImage_GetHeight(bitmap);
		if (w < width || h < height)
		{
			FIBITMAP *newBitmap = FreeImage_Rescale(bitmap, width, height, FREE_IMAGE_FILTER::FILTER_BICUBIC);
			if (newBitmap == nullptr)
			{
				return false;
			}

			ubitmap.reset(newBitmap);
			bitmap = newBitmap;
		}
	}

	const mu_uint16 numLayers = static_cast<mu_uint16>(bitmaps.size());
	const auto device = MUGraphics::GetDevice();

	// Textures
	{
		std::vector<Diligent::TextureSubResData> subresources;
		for (auto iter = bitmaps.begin(); iter != bitmaps.end(); ++iter)
		{
			auto &bitmap = *iter;
			Diligent::TextureSubResData subresource;
			subresource.pData = FreeImage_GetBits(bitmap.get());
			subresource.Stride = width * sizeof(mu_uint8) * 4;
			subresources.push_back(subresource);
		}

		Diligent::TextureDesc textureDesc;
		textureDesc.Type = Diligent::RESOURCE_DIM_TEX_2D_ARRAY;
		textureDesc.Width = width;
		textureDesc.Height = height;
		textureDesc.ArraySize = numLayers;
		textureDesc.Format = Diligent::TEX_FORMAT_RGBA8_UNORM;
		textureDesc.Usage = Diligent::USAGE_IMMUTABLE;
		textureDesc.BindFlags = Diligent::BIND_SHADER_RESOURCE;

		Diligent::TextureData textureData(subresources.data(), static_cast<mu_uint32>(subresources.size()));
		Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
		device->CreateTexture(textureDesc, &textureData, &texture);
		if (texture == nullptr)
		{
			return false;
		}

		texture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE)->SetSampler(GetTextureSampler(MUTextures::CalculateSamplerFlags(filter, wrap)));
		GrassTextures = texture;
	}

	// UV Textures
	{
		const mu_uint32 settingsWidth = GetPowerOfTwoSize(static_cast<mu_uint32>(settings.size()));
		const mu_size memorySize = settingsWidth * sizeof(SettingFormat);
		std::unique_ptr<mu_uint8[]> memory(new (std::nothrow) mu_uint8[memorySize]);
		mu_zeromem(memory.get(), memorySize);
		mu_memcpy(memory.get(), settings.data(), settings.size() * sizeof(SettingFormat));

		std::vector<Diligent::TextureSubResData> subresources;
		Diligent::TextureSubResData subresource;
		subresource.pData = memory.get();
		subresource.Stride = settingsWidth * sizeof(SettingFormat);
		subresources.push_back(subresource);

		Diligent::TextureDesc textureDesc;
		textureDesc.Type = Diligent::RESOURCE_DIM_TEX_2D;
		textureDesc.Width = settingsWidth;
		textureDesc.Height = 1;
		textureDesc.ArraySize = 1;
		textureDesc.Format = Diligent::TEX_FORMAT_R32_FLOAT;
		textureDesc.Usage = Diligent::USAGE_IMMUTABLE;
		textureDesc.BindFlags = Diligent::BIND_SHADER_RESOURCE;

		Diligent::TextureData textureData(subresources.data(), static_cast<mu_uint32>(subresources.size()));
		Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
		device->CreateTexture(textureDesc, &textureData, &texture);
		if (texture == nullptr)
		{
			return false;
		}

		GrassUVTexture = texture;
	}

	return true;
}

const mu_uint32 GetMapValue(
	const std::map<mu_uint32, mu_uint32> map,
	const mu_uint32 value
)
{
	auto iter = map.find(value);
	if (iter == map.end()) return NInvalidUInt32;
	return iter->second;
}

const mu_boolean NTerrain::LoadMappings(
	mu_utf8string path,
	const std::map<mu_uint32, mu_uint32> &texturesMap,
	const std::map<mu_uint32, mu_uint32> &grassTexturesMap
)
{
	NormalizePath(path);

	auto ext = path.substr(path.find_last_of('.') + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), mu_utf8tolower);

	SDL_RWops *fp = nullptr;
	if (mu_rwfromfile<EGameDirectoryType::eSupport>(&fp, path, "rb") == false)
	{
		mu_error("heightmap not found ({})", path);
		return false;
	}

	mu_isize fileLength = static_cast<mu_isize>(SDL_RWsize(fp));
	std::unique_ptr<mu_uint8[]> buffer(new_nothrow mu_uint8[fileLength]);
	SDL_RWread(fp, buffer.get(), fileLength, 1);
	SDL_RWclose(fp);

	XorDecrypt(buffer.get(), buffer.get(), static_cast<mu_uint32>(fileLength));

	NBinaryReader reader(buffer.get(), static_cast<mu_uint32>(fileLength));
	mu_uint8 version = reader.Read<mu_uint8>();
	mu_int32 map = static_cast<mu_int32>(reader.Read<mu_uint8>());
	const mu_uint8 *mapping1 = reader.GetPointer(); reader.Skip(TerrainSize * TerrainSize);
	const mu_uint8 *mapping2 = reader.GetPointer(); reader.Skip(TerrainSize * TerrainSize);
	const mu_uint8 *alpha = reader.GetPointer(); reader.Skip(TerrainSize * TerrainSize);

	std::unique_ptr<mu_uint8[]> memory(new (std::nothrow) mu_uint8[TerrainSize * TerrainSize * sizeof(MappingFormat)]);
	MappingFormat *mapping = reinterpret_cast<MappingFormat *>(memory.get());

	for (mu_uint32 index = 0; index < TerrainSize * TerrainSize; ++index)
	{
		mu_uint8 map1 = GetMapValue(texturesMap, mapping1[index]);
		mu_uint8 map2 = GetMapValue(texturesMap, mapping2[index]);
		mapping[index] = MappingFormat(
			map1,
			map2,
			map1 == NInvalidUInt8 || map2 == NInvalidUInt8
			? 0
			: alpha[index],
			GetMapValue(grassTexturesMap, mapping1[index])
		);
	}

	const auto device = MUGraphics::GetDevice();

	std::vector<Diligent::TextureSubResData> subresources;
	Diligent::TextureSubResData subresource;
	subresource.pData = memory.get();
	subresource.Stride = TerrainSize * sizeof(mu_uint8) * 4;
	subresources.push_back(subresource);

	Diligent::TextureDesc textureDesc;
	textureDesc.Type = Diligent::RESOURCE_DIM_TEX_2D;
	textureDesc.Width = TerrainSize;
	textureDesc.Height = TerrainSize;
	textureDesc.Format = Diligent::TEX_FORMAT_RGBA8_UINT;
	textureDesc.Usage = Diligent::USAGE_IMMUTABLE;
	textureDesc.BindFlags = Diligent::BIND_SHADER_RESOURCE;

	Diligent::TextureData textureData(subresources.data(), static_cast<mu_uint32>(subresources.size()));
	Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
	device->CreateTexture(textureDesc, &textureData, &texture);
	if (texture == nullptr)
	{
		return false;
	}

	MappingTexture = texture;

	return true;
}

const mu_boolean NTerrain::LoadAttributes(mu_utf8string path)
{
	NormalizePath(path);

	constexpr mu_uint32 AttributeV1Size = 65540;
	constexpr mu_uint32 AttributeV2Size = 131076;

	auto ext = path.substr(path.find_last_of('.') + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), mu_utf8tolower);

	SDL_RWops *fp = nullptr;
	if (mu_rwfromfile<EGameDirectoryType::eSupport>(&fp, path, "rb") == false)
	{
		mu_error("heightmap not found ({})", path);
		return false;
	}

	mu_isize fileLength = static_cast<mu_isize>(SDL_RWsize(fp));

	// Well, what a shitty if
	if (
		fileLength != static_cast<mu_int64>(AttributeV1Size) &&
		fileLength != static_cast<mu_int64>(AttributeV2Size)
		)
	{
		mu_error("invalid attributes size ({})", path);
		return false;
	}

	const mu_boolean isExtended = fileLength == static_cast<mu_int64>(AttributeV2Size);

	std::unique_ptr<mu_uint8[]> buffer(new_nothrow mu_uint8[fileLength]);
	SDL_RWread(fp, buffer.get(), fileLength, 1);
	SDL_RWclose(fp);

	XorDecrypt(buffer.get(), buffer.get(), static_cast<mu_uint32>(fileLength));
	BuxConvert(buffer.get(), static_cast<mu_uint32>(fileLength));

	NBinaryReader reader(buffer.get(), static_cast<mu_uint32>(fileLength));
	mu_uint8 version = reader.Read<mu_uint8>();
	mu_int32 map = static_cast<mu_int32>(reader.Read<mu_uint8>());
	mu_uint32 width = static_cast<mu_uint32>(reader.Read<mu_uint8>());
	mu_uint32 height = static_cast<mu_uint32>(reader.Read<mu_uint8>());

	if (version != 0 || width != 255 || height != 255)
	{
		mu_error("invalid attributes values ({})", path);
		return false;
	}
	
	if (!TerrainAttributes) TerrainAttributes.reset(new_nothrow TerrainAttribute::Type[TerrainSize * TerrainSize]);
	if (isExtended)
	{
		constexpr mu_uint32 AttributesSize = TerrainSize * TerrainSize * sizeof(mu_uint16);
		mu_memcpy(TerrainAttributes.get(), reader.GetPointer(), AttributesSize);
		reader.Skip(AttributesSize);
	}
	else
	{
		constexpr mu_uint32 AttributesSize = TerrainSize * TerrainSize;
		const mu_uint8 *attributes = reader.GetPointer(); reader.Skip(AttributesSize);
		for (mu_uint32 index = 0; index < AttributesSize; ++index)
		{
			TerrainAttributes[index] = static_cast<mu_uint16>(attributes[index]);
		}
	}

	const auto device = MUGraphics::GetDevice();

	std::vector<Diligent::TextureSubResData> subresources;
	Diligent::TextureSubResData subresource;
	subresource.pData = TerrainAttributes.get();
	subresource.Stride = TerrainAttribute::Stride;
	subresources.push_back(subresource);

	Diligent::TextureDesc textureDesc;
	textureDesc.Type = Diligent::RESOURCE_DIM_TEX_2D;
	textureDesc.Width = TerrainSize;
	textureDesc.Height = TerrainSize;
	textureDesc.Format = TerrainAttribute::Format;
	textureDesc.Usage = Diligent::USAGE_DEFAULT;
	textureDesc.BindFlags = Diligent::BIND_SHADER_RESOURCE;

	Diligent::TextureData textureData(subresources.data(), static_cast<mu_uint32>(subresources.size()));
	Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
	device->CreateTexture(textureDesc, &textureData, &texture);
	if (texture == nullptr)
	{
		return false;
	}

	AttributesTexture = texture;

	return true;
}

const mu_boolean NTerrain::PrepareSettings(const mu_utf8string path, const nlohmann::json document)
{
	const auto water = document["water"];
	if (water.is_object() == false)
	{
		mu_error("terrain.json malformed ({})", path + "terrain.json");
		return false;
	}

	WaterModulus = water["mod"].get<mu_float>();
	WaterMultiplier = water["mul"].get<mu_float>();

	const auto wind = document["wind"];
	if (wind.is_object() == false)
	{
		mu_error("terrain.json malformed ({})", path + "terrain.json");
		return false;
	}

	WindScale = wind["scale"].get<mu_float>();
	WindModulus = wind["mod"].get<mu_float>();
	WindMultiplier = wind["mul"].get<mu_float>();

	const auto device = MUGraphics::GetDevice();

	Diligent::BufferDesc bufferDesc;
	bufferDesc.Usage = Diligent::USAGE_DYNAMIC;
	bufferDesc.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
	bufferDesc.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
	bufferDesc.Size = sizeof(TerrainSettings);

	Diligent::RefCntAutoPtr<Diligent::IBuffer> buffer;
	device->CreateBuffer(bufferDesc, nullptr, &buffer);
	if (buffer == nullptr)
	{
		return false;
	}

	SettingsUniform = buffer;

	return true;
}

const mu_boolean NTerrain::GenerateBuffers()
{
	/*
		TODO:
		Since all terrains are 256x256 we can make the vertex and index buffer static to use it for all terrains.
		After implement occlussion culling the index buffer should be dynamic.
	*/
	InitializeTerrainVertices();
	InitializeTerrainIndexes();

	const auto device = MUGraphics::GetDevice();

	// Vertex Buffer
	{
		Diligent::BufferDesc bufferDesc;
		bufferDesc.Usage = Diligent::USAGE_DEFAULT;
		bufferDesc.BindFlags = Diligent::BIND_VERTEX_BUFFER; // We do not really bind the buffer, but D3D11 wants at least one bind flag bit
		bufferDesc.Size = sizeof(TerrainVertices);

		Diligent::BufferData bufferData;
		bufferData.pData = TerrainVertices;
		bufferData.DataSize = sizeof(TerrainVertices);

		Diligent::RefCntAutoPtr<Diligent::IBuffer> buffer;
		device->CreateBuffer(bufferDesc, &bufferData, &buffer);
		if (buffer == nullptr)
		{
			return false;
		}

		VertexBuffer = buffer;
	}

	// Index Buffer
	{
		Diligent::BufferDesc bufferDesc;
		bufferDesc.Usage = Diligent::USAGE_DEFAULT;
		bufferDesc.BindFlags = Diligent::BIND_INDEX_BUFFER; // We do not really bind the buffer, but D3D11 wants at least one bind flag bit
		bufferDesc.Size = sizeof(TerrainIndexes);

		Diligent::BufferData bufferData;
		bufferData.pData = TerrainIndexes;
		bufferData.DataSize = sizeof(TerrainIndexes);

		Diligent::RefCntAutoPtr<Diligent::IBuffer> buffer;
		device->CreateBuffer(bufferDesc, &bufferData, &buffer);
		if (buffer == nullptr)
		{
			return false;
		}

		IndexBuffer = buffer;
	}

	return true;
}

const mu_boolean NTerrain::PreparePipelines()
{
	// Terrain
	{
		const auto swapchain = MUGraphics::GetSwapChain();
		const auto &swapchainDesc = swapchain->GetDesc();

		NFixedPipelineState fixedState;
		fixedState.CombinedShader = Program;
		fixedState.RTVFormat = swapchainDesc.ColorBufferFormat;
		fixedState.DSVFormat = swapchainDesc.DepthBufferFormat;

		NDynamicPipelineState dynamicState;

		TerrainPipeline = GetPipelineState(fixedState, dynamicState);
		if (TerrainPipeline->StaticInitialized == false)
		{
			TerrainPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "ModelViewProj")->Set(MURenderState::GetViewProjUniform());
			TerrainPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "g_HeightTexture")->Set(HeightmapTexture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
			TerrainPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "g_LightTexture")->Set(LightmapTexture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
			TerrainPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "g_NormalTexture")->Set(NormalTexture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
			TerrainPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "g_MappingTexture")->Set(MappingTexture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
			TerrainPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "g_UVTexture")->Set(UVTexture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
			TerrainPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "g_AttributesTexture")->Set(AttributesTexture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
			TerrainPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "TerrainSettings")->Set(SettingsUniform);
			TerrainPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_PIXEL, "g_Textures")->Set(Textures->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
			TerrainPipeline->StaticInitialized = true;
		}

		NResourceId resourceIds[1] = { NInvalidUInt32 };
		TerrainBinding = GetShaderBinding(TerrainPipeline, mu_countof(resourceIds), resourceIds);
		TerrainBinding->Initialized = true;
	}

	// Grass
	{
		const auto swapchain = MUGraphics::GetSwapChain();
		const auto &swapchainDesc = swapchain->GetDesc();

		NFixedPipelineState fixedState;
		fixedState.CombinedShader = GrassProgram;
		fixedState.RTVFormat = swapchainDesc.ColorBufferFormat;
		fixedState.DSVFormat = swapchainDesc.DepthBufferFormat;

		NDynamicPipelineState dynamicState;
		dynamicState.CullMode = Diligent::CULL_MODE_NONE;
		dynamicState.AlphaWrite = false;
		dynamicState.DepthWrite = false;
		dynamicState.SrcBlend = Diligent::BLEND_FACTOR_SRC_ALPHA;
		dynamicState.DestBlend = Diligent::BLEND_FACTOR_INV_SRC_ALPHA;
		dynamicState.BlendOp = Diligent::BLEND_OPERATION_ADD;

		GrassPipeline = GetPipelineState(fixedState, dynamicState);
		if (GrassPipeline->StaticInitialized == false)
		{
			GrassPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "ModelViewProj")->Set(MURenderState::GetViewProjUniform());
			GrassPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "g_HeightTexture")->Set(HeightmapTexture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
			GrassPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "g_LightTexture")->Set(LightmapTexture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
			GrassPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "g_NormalTexture")->Set(NormalTexture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
			GrassPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "g_MappingTexture")->Set(MappingTexture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
			GrassPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "g_UVTexture")->Set(GrassUVTexture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
			GrassPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "g_AttributesTexture")->Set(AttributesTexture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
			GrassPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, "TerrainSettings")->Set(SettingsUniform);
			GrassPipeline->Pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_PIXEL, "g_Textures")->Set(GrassTextures->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
			GrassPipeline->StaticInitialized = true;
		}

		NResourceId resourceIds[1] = { NInvalidUInt32 };
		GrassBinding = GetShaderBinding(GrassPipeline, mu_countof(resourceIds), resourceIds);
		GrassBinding->Initialized = true;
	}

	return true;
}

void NTerrain::Reset()
{
	// restore primary light per update frame
	mu_memcpy(TerrainPrimaryLight.get(), TerrainLight.get(), sizeof(glm::vec3) * TerrainSize * TerrainSize);
}

void NTerrain::ConfigureUniforms()
{
	const auto immediateContext = MURenderState::GetImmediateContext();

	Settings.WaterMove = glm::mod(MUState::GetWorldTime(), WaterModulus) * WaterMultiplier;
	Settings.WindScale = 10.0f;
	Settings.WindSpeed = glm::mod(MUState::GetWorldTime(), WindModulus) * WindMultiplier;
	Settings.Dummy = 0.0f;

	// Update Settings Uniform
	{
		Diligent::MapHelper<TerrainSettings> terrainSettings(immediateContext, SettingsUniform, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD);
		TerrainSettings *buffer = terrainSettings;
		mu_memcpy(buffer, &Settings, sizeof(TerrainSettings));
	}
}

void NTerrain::Update()
{
	const auto immediateContext = MURenderState::GetImmediateContext();
	immediateContext->UpdateTexture(
		LightmapTexture,
		0, 0,
		Diligent::Box(0, TerrainSize, 0, TerrainSize),
		Diligent::TextureSubResData(LightmapMemory.get(), TerrainSize * sizeof(mu_uint8) * 4),
		Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
		Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION
	);
	immediateContext->UpdateTexture(
		NormalTexture,
		0, 0,
		Diligent::Box(0, TerrainSize, 0, TerrainSize),
		Diligent::TextureSubResData(NormalMemory.get(), TerrainSize * sizeof(mu_uint16) * 4),
		Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
		Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION
	);
	immediateContext->UpdateTexture(
		AttributesTexture,
		0, 0,
		Diligent::Box(0, TerrainSize, 0, TerrainSize),
		Diligent::TextureSubResData(TerrainAttributes.get(), TerrainAttribute::Stride),
		Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
		Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION
	);
}

void NTerrain::Render()
{
	const auto renderManager = MUGraphics::GetRenderManager();
	const auto immediateContext = MURenderState::GetImmediateContext();

	renderManager->SetVertexBuffer(
		RSetVertexBuffer{
			.StartSlot = 0,
			.Buffer = VertexBuffer.RawPtr(),
			.Offset = 0,
			.StateTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
			.Flags = Diligent::SET_VERTEX_BUFFERS_FLAG_RESET,
		}
	);
	renderManager->SetIndexBuffer(
		RSetIndexBuffer{
			.IndexBuffer = IndexBuffer,
			.ByteOffset = 0,
			.StateTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
		}
	);
	renderManager->SetPipelineState(TerrainPipeline);
	renderManager->CommitShaderResources(
		RCommitShaderResources{
			.ShaderResourceBinding = TerrainBinding,
			.StateTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
		}
	);
	renderManager->DrawIndexed(
		RDrawIndexed{
			.Attribs = Diligent::DrawIndexedAttribs(NumTerrainIndexes, Diligent::VT_UINT32, Diligent::DRAW_FLAG_VERIFY_ALL)
		},
		RCommandListInfo{
			.Type = NDrawOrderType::Classifier,
			.Classify = NRenderClassify::Opaque,
			.View = 0,
			.Index = 1,
		}
	);

	if (GrassUVTexture != nullptr)
	{
		renderManager->SetVertexBuffer(
			RSetVertexBuffer{
				.StartSlot = 0,
				.Buffer = VertexBuffer.RawPtr(),
				.Offset = 0,
				.StateTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
				.Flags = Diligent::SET_VERTEX_BUFFERS_FLAG_RESET,
			}
		);
		renderManager->SetIndexBuffer(
			RSetIndexBuffer{
				.IndexBuffer = IndexBuffer,
				.ByteOffset = 0,
				.StateTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
			}
		);
		renderManager->SetPipelineState(GrassPipeline);
		renderManager->CommitShaderResources(
			RCommitShaderResources{
				.ShaderResourceBinding = GrassBinding,
				.StateTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
			}
		);
		renderManager->DrawIndexed(
			RDrawIndexed{
				.Attribs = Diligent::DrawIndexedAttribs(NumTerrainIndexes, Diligent::VT_UINT32, Diligent::DRAW_FLAG_VERIFY_ALL)
			},
			RCommandListInfo{
				.Type = NDrawOrderType::Classifier,
				.Classify = NRenderClassify::PreAlpha,
				.View = 0,
				.Index = 1,
			}
		);
	}
}

Diligent::ITexture *NTerrain::GetLightmapTexture()
{
	return LightmapTexture.RawPtr();
}

const mu_float NTerrain::GetHeight(const mu_uint32 x, const mu_uint32 y)
{
	return TerrainHeight[GetTerrainMaskIndex(x, y)];
}

const glm::vec3 NTerrain::GetLight(const mu_uint32 x, const mu_uint32 y)
{
	return TerrainLight[GetTerrainMaskIndex(x, y)];
}

const glm::vec3 NTerrain::GetPrimaryLight(const mu_uint32 x, const mu_uint32 y)
{
	return TerrainPrimaryLight[GetTerrainMaskIndex(x, y)];
}

const glm::vec3 NTerrain::GetNormal(const mu_uint32 x, const mu_uint32 y)
{
	return TerrainNormal[GetTerrainMaskIndex(x, y)];
}

const TerrainAttribute::Type NTerrain::GetAttribute(const mu_uint32 x, const mu_uint32 y)
{
	return TerrainAttributes[GetTerrainMaskIndex(x, y)];
}

const glm::vec3 NTerrain::CalculatePrimaryLight(mu_float x, mu_float y)
{
	x *= TerrainScaleInv;
	y *= TerrainScaleInv;

	const mu_int32 xi = static_cast<mu_int32>(x);
	const mu_int32 yi = static_cast<mu_int32>(y);
	if (xi < 0 || xi >= TerrainMask || yi < 0 || yi >= TerrainMask)
	{
		return glm::vec3(0.0f, 0.0f, 0.0f);
	}

	const mu_uint32 index1 = GetTerrainIndex(xi, yi);
	const mu_uint32 index2 = GetTerrainIndex(xi + 1, yi);
	const mu_uint32 index3 = GetTerrainIndex(xi + 1, yi + 1);
	const mu_uint32 index4 = GetTerrainIndex(xi, yi + 1);

	mu_float dx = glm::mod(x, 1.0f);
	mu_float dy = glm::mod(y, 1.0f);
	glm::vec3 light;
	for (mu_uint32 i = 0; i < 3; ++i)
	{
		mu_float left = TerrainPrimaryLight[index1][i] + (TerrainPrimaryLight[index4][i] - TerrainPrimaryLight[index1][i]) * dy;
		mu_float right = TerrainPrimaryLight[index2][i] + (TerrainPrimaryLight[index3][i] - TerrainPrimaryLight[index2][i]) * dy;
		light[i] = (left + (right - left) * dx);
	}

	return light;
}

const glm::vec3 NTerrain::CalculateBackLight(mu_float x, mu_float y)
{
	x *= TerrainScaleInv;
	y *= TerrainScaleInv;

	const mu_int32 xi = static_cast<mu_int32>(x);
	const mu_int32 yi = static_cast<mu_int32>(y);
	if (xi < 0 || xi >= TerrainMask || yi < 0 || yi >= TerrainMask)
	{
		return glm::vec3(0.0f, 0.0f, 0.0f);
	}

	const mu_uint32 index1 = GetTerrainIndex(xi, yi);
	const mu_uint32 index2 = GetTerrainIndex(xi + 1, yi);
	const mu_uint32 index3 = GetTerrainIndex(xi + 1, yi + 1);
	const mu_uint32 index4 = GetTerrainIndex(xi, yi + 1);

	mu_float dx = glm::mod(x, 1.0f);
	mu_float dy = glm::mod(y, 1.0f);
	glm::vec3 light;
	for (mu_uint32 i = 0; i < 3; ++i)
	{
		mu_float left = TerrainLight[index1][i] + (TerrainLight[index4][i] - TerrainLight[index1][i]) * dy;
		mu_float right = TerrainLight[index2][i] + (TerrainLight[index3][i] - TerrainLight[index2][i]) * dy;
		light[i] = (left + (right - left) * dx);
	}

	return light;
}