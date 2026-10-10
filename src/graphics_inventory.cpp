// Copyright (c) 2026 Dogukan Sahil
// Modified for DoWATT v1.0 on 2026-10-10. See LICENSE and NVML-EXCEPTION.txt.
// Additional permission under GPL v3 section 7: linking with installed NVIDIA NVML
// is permitted as described in NVML-EXCEPTION.txt.
// SPDX-License-Identifier: GPL-3.0-only
#define WIN32_LEAN_AND_MEAN
#include "graphics_inventory.h"
#include <initguid.h>
#include <dxgi1_2.h>
#include <d3d12.h>
typedef HRESULT (WINAPI *CreateFactory)(REFIID, void **);
typedef HRESULT (WINAPI *CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
static UINT DeviceId(const wchar_t *pnp) {
    for (UINT i = 0; pnp[i]; ++i) if (pnp[i] == L'D' && pnp[i+1] == L'E' && pnp[i+2] == L'V' && pnp[i+3] == L'_') {
        UINT value = 0;
        for (UINT d = 0; d < 4; ++d) {
            wchar_t c = pnp[i+4+d]; UINT nibble;
            if (c >= L'0' && c <= L'9') nibble = c-L'0';
            else if (c >= L'A' && c <= L'F') nibble = c-L'A'+10;
            else if (c >= L'a' && c <= L'f') nibble = c-L'a'+10;
            else return MAXUINT;
            value = value*16+nibble;
        }
        return value;
    }
    return MAXUINT;
}
BOOL IntelGpuIsUma(const wchar_t *description, const wchar_t *pnp) {
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE d3d = LoadLibraryExW(L"d3d12.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    BOOL integrated = FALSE;
    if (dxgi && d3d) {
        CreateFactory createFactory = (CreateFactory)GetProcAddress(dxgi, "CreateDXGIFactory1");
        CreateDevice createDevice = (CreateDevice)GetProcAddress(d3d, "D3D12CreateDevice");
        IDXGIFactory1 *factory = 0;
        if (createFactory && createDevice && SUCCEEDED(createFactory(IID_IDXGIFactory1, (void **)&factory))) {
            UINT expected = DeviceId(pnp);
            for (UINT i = 0; i < 64; ++i) {
                IDXGIAdapter1 *adapter = 0;
                HRESULT hr = factory->EnumAdapters1(i, &adapter);
                if (FAILED(hr)) break;
                DXGI_ADAPTER_DESC1 desc = {};
                if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x8086 &&
                    !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    ((expected != MAXUINT && desc.DeviceId == expected) ||
                     (expected == MAXUINT && lstrcmpiW(desc.Description, description) == 0))) {
                    ID3D12Device *device = 0;
                    if (SUCCEEDED(createDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, (void **)&device))) {
                        D3D12_FEATURE_DATA_ARCHITECTURE1 architecture = {};
                        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &architecture, sizeof(architecture))))
                            integrated = architecture.UMA;
                        device->Release();
                    }
                }
                adapter->Release();
                if (integrated) break;
            }
            factory->Release();
        }
    }
    if (d3d) FreeLibrary(d3d); if (dxgi) FreeLibrary(dxgi);
    return integrated;
}
