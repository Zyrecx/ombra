#include "ombra-ui/ui/fonts.h"

#include <windows.h>

#include <mutex>

namespace ombra_ui {

using Microsoft::WRL::ComPtr;

UiFont UiFontFromKey(const std::string& key) {
    if (key == "system") return UiFont::System;
    if (key == "geist") return UiFont::Geist;
    return UiFont::Inter;
}

namespace {

// The resource bytes live as long as the module, so DirectWrite may reference them without a copy;
// a null owner would make it copy each file instead.
struct StaticOwner final : IUnknown {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown)) { *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
};
StaticOwner g_owner;

ComPtr<IDWriteFontCollection1> Build(IDWriteFactory* factory, std::initializer_list<int> ids) {
    ComPtr<IDWriteFactory5> f5;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&f5)))) return nullptr;
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    if (FAILED(f5->CreateInMemoryFontFileLoader(&loader)) || FAILED(f5->RegisterFontFileLoader(loader.Get())))
        return nullptr;
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(f5->CreateFontSetBuilder(&builder))) return nullptr;
    const HMODULE module = GetModuleHandleW(nullptr);
    int added = 0;
    for (int id : ids) {
        HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
        HGLOBAL h = res ? LoadResource(module, res) : nullptr;
        const void* data = h ? LockResource(h) : nullptr;
        if (!data) continue;
        ComPtr<IDWriteFontFile> file;
        if (SUCCEEDED(loader->CreateInMemoryFontFileReference(f5.Get(), data, SizeofResource(module, res), &g_owner,
                                                              &file)) &&
            SUCCEEDED(builder->AddFontFile(file.Get())))
            ++added;
    }
    if (added == 0) return nullptr;
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> collection;
    if (FAILED(builder->CreateFontSet(&set)) || FAILED(f5->CreateFontCollectionFromFontSet(set.Get(), &collection)))
        return nullptr;
    return collection;
}

}  // namespace

IDWriteFontCollection* EmbeddedFontCollection(IDWriteFactory* factory, UiFont font) {
    if (!factory || font == UiFont::System) return nullptr;
    // The shared factory is one object per process, so one collection per font serves every window.
    // Built on first use only: a user on System or Geist never loads Inter.
    static std::mutex mutex;
    static ComPtr<IDWriteFactory> owner;
    static ComPtr<IDWriteFontCollection1> inter, geist;
    static bool tried_inter = false, tried_geist = false;  // a missing resource isn't retried per format
    std::lock_guard<std::mutex> lock(mutex);
    if (owner.Get() != factory) {
        owner = factory;
        inter.Reset();
        geist.Reset();
        tried_inter = tried_geist = false;
    }
    ComPtr<IDWriteFontCollection1>& slot = font == UiFont::Geist ? geist : inter;
    bool& tried = font == UiFont::Geist ? tried_geist : tried_inter;
    if (!tried) {
        tried = true;
        slot = font == UiFont::Geist ? Build(factory, {OMBRA_RC_FONT_GEIST_REGULAR, OMBRA_RC_FONT_GEIST_SEMIBOLD})
                                     : Build(factory, {OMBRA_RC_FONT_INTER_REGULAR, OMBRA_RC_FONT_INTER_SEMIBOLD});
    }
    return slot.Get();
}

}  // namespace ombra_ui
