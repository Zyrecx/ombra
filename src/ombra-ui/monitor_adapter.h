// The DXGI adapter that drives a monitor, so a window on it renders on that GPU. Drawing on
// another adapter makes DWM copy every frame across GPUs and, on a hybrid laptop, wakes the dGPU.
#pragma once

#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace ombra_ui {

// Null when no adapter reports an output for `mon`; callers then use the default adapter.
inline Microsoft::WRL::ComPtr<IDXGIAdapter1> AdapterForMonitor(HMONITOR mon) {
    using Microsoft::WRL::ComPtr;
    if (!mon) return nullptr;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        ComPtr<IDXGIOutput> output;
        for (UINT j = 0; adapter->EnumOutputs(j, &output) != DXGI_ERROR_NOT_FOUND; ++j) {
            DXGI_OUTPUT_DESC od{};
            if (SUCCEEDED(output->GetDesc(&od)) && od.Monitor == mon) return adapter;
            output.Reset();
        }
        adapter.Reset();
    }
    return nullptr;
}

}  // namespace ombra_ui
