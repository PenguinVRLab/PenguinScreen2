// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/RedtapeWindows.h"
#include "common/RedtapeWilCom.h"

#include "Config.h"
#include "GS/GS.h"

#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <string>
#include <string_view>
#include <vector>

namespace D3D
{
	wil::com_ptr_nothrow<IDXGIFactory5> CreateFactory(bool debug);

	std::vector<GSAdapterInfo> GetAdapterInfo(IDXGIFactory5* factory);

	bool GetRequestedExclusiveFullscreenModeDesc(IDXGIFactory5* factory, HWND window_hwnd, u32 width, u32 height,
		float refresh_rate, DXGI_FORMAT format, DXGI_MODE_DESC* fullscreen_mode, IDXGIOutput** output);

	wil::com_ptr_nothrow<IDXGIAdapter1> GetAdapterByName(IDXGIFactory5* factory, const std::string_view name);

	wil::com_ptr_nothrow<IDXGIAdapter1> GetFirstAdapter(IDXGIFactory5* factory);

	wil::com_ptr_nothrow<IDXGIAdapter1> GetChosenOrFirstAdapter(IDXGIFactory5* factory, const std::string_view name);

	std::string GetAdapterName(IDXGIAdapter1* adapter);

	std::string GetDriverVersionFromLUID(const LUID& luid);

	enum class VendorID
	{
		Unknown,
		Nvidia,
		AMD,
		Intel
	};

	VendorID GetVendorID(IDXGIAdapter1* adapter);
	GSRendererType GetPreferredRenderer();

	enum class ShaderType
	{
		Vertex,
		Pixel,
		Compute
	};

	enum class ShaderModel
	{
		SM40 = 0x40,
		SM41 = 0x41,
		SM50 = 0x50,
		SM51 = 0x51,
	};

	const char* ShaderModelToCacheString(ShaderModel shader_model);

	wil::com_ptr_nothrow<ID3DBlob> CompileShader(ShaderType type, ShaderModel shader_model, bool debug,
		const std::string_view code, const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main");
};
