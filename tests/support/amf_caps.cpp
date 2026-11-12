#include "support/amf_caps.h"

#include "ombra/amf_encoder.h"
#include "ombra/amf_probe.h"
#include "ombra/gputopo.h"

namespace ombra::test {

bool AmfAv1AlignmentInProcess(const EncodeAdapter& adapter, uint32_t& align_w, uint32_t& align_h) {
    std::string        error;
    const AmfRuntime*  rt = LoadAmfRuntime(&error);
    if (!rt) return false;
    winrt::com_ptr<ID3D11Device>        device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    if (!CreateD3D11DeviceOnAdapter(adapter.luid, device, context, error)) return false;
    amf::AMFContextPtr amf_context;
    if (rt->factory->CreateContext(&amf_context) != AMF_OK) return false;
    if (amf_context->InitDX11(device.get()) != AMF_OK) return false;
    const bool ok = AmfAv1Alignment(*rt, amf_context, align_w, align_h);
    amf_context->Terminate();
    return ok;
}

void SeedAmfCapsInProcess() {
    std::vector<AmfAdapterCaps> caps;
    for (const auto& a : EnumerateAdapters(nullptr)) {
        if (a.vendor_id != kVendorAmd || a.software) continue;
        AmfAdapterCaps c;
        c.luid = a.luid;
        c.h264 = true;
        c.av1  = AmfAv1AlignmentInProcess(EncodeAdapter{EncoderBackend::Amf, a.luid, a.description}, c.av1_align_w,
                                          c.av1_align_h);
        caps.push_back(c);
    }
    SetAmfAdapterCaps(caps);
}

std::vector<AmfAdapterCaps> AllAmdH264Caps() {
    std::vector<AmfAdapterCaps> caps;
    for (const auto& a : EnumerateAdapters(nullptr)) {
        if (a.vendor_id != kVendorAmd || a.software) continue;
        AmfAdapterCaps c;
        c.luid = a.luid;
        c.h264 = true;
        caps.push_back(c);
    }
    return caps;
}

}  // namespace ombra::test
