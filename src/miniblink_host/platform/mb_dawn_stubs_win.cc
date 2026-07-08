// mb_dawn_stubs_win.cc — satisfy the Windows D3D shared-image backing's Dawn
// references when the library is built WITHOUT WebGPU (use_dawn=false).
//
// d3d_image_backing.cc weaves Dawn/Graphite support through its D3D11 paths
// far more deeply than the mac IOSurface backing that patch 0017 gates at
// compile time — gating it would be a ~30-hunk donor patch across members,
// virtual overrides and access bookkeeping. Every symbol below is reachable
// only through a live wgpu::Device, which cannot exist in this configuration
// (dawn::native is not linked, so no Dawn instance/device can ever be
// created; the WebGPU decoder is off; window.GPU is absent). Two stub
// flavors, chosen per symbol:
//
//  - DawnSharedTextureCache is constructed EAGERLY by every D3DImageBacking
//    (ctor-initializer), so its ctor/dtor and device-keyed lookups are real
//    but trivially empty-cache semantics — never a trap.
//  - The representation classes and dawn::native accessors need a device to
//    be called at all: base::ImmediateCrash(), loudly, if one ever fires.
//
// GN-gated: only in the miniblink_host target when (is_win && !use_dawn).

#include <dawn/native/D3D11Backend.h>
#include <dawn/native/D3D12Backend.h>
#include <dawn/native/DawnNative.h>

#include "base/immediate_crash.h"
#include "gpu/command_buffer/service/dawn_context_provider.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "gpu/command_buffer/service/shared_image/dawn_egl_image_representation.h"
#include "gpu/command_buffer/service/shared_image/dawn_shared_texture_cache.h"
#include "gpu/command_buffer/service/shared_image/skia_graphite_dawn_image_representation.h"

namespace dawn::native {

bool IsTextureSubresourceInitialized(WGPUTexture,
                                     uint32_t,
                                     uint32_t,
                                     uint32_t,
                                     uint32_t,
                                     WGPUTextureAspect) {
  base::ImmediateCrash();
}

namespace d3d11 {
Microsoft::WRL::ComPtr<ID3D11Device> GetD3D11Device(WGPUDevice) {
  base::ImmediateCrash();
}
}  // namespace d3d11

namespace d3d12 {
Microsoft::WRL::ComPtr<ID3D12Device> GetD3D12Device(WGPUDevice) {
  base::ImmediateCrash();
}
Microsoft::WRL::ComPtr<ID3D11On12Device> GetOrCreateD3D11On12Device(
    WGPUDevice) {
  base::ImmediateCrash();
}
}  // namespace d3d12

}  // namespace dawn::native

namespace gpu {

// ---- DawnSharedTextureCache: real (empty-cache) semantics ------------------
// Constructed per-backing; with no Dawn device the maps just stay empty.

DawnSharedTextureCache::DawnSharedTextureCache() = default;
DawnSharedTextureCache::~DawnSharedTextureCache() = default;

DawnSharedTextureCache::TextureMetadata::TextureMetadata(
    wgpu::TextureUsage usage,
    wgpu::TextureUsage internal_usage,
    const std::vector<wgpu::TextureFormat>& view_formats)
    : usage(usage),
      internal_usage(internal_usage),
      view_formats(view_formats) {}
DawnSharedTextureCache::TextureMetadata::~TextureMetadata() = default;
DawnSharedTextureCache::TextureMetadata::TextureMetadata(TextureMetadata&&) =
    default;
DawnSharedTextureCache::TextureMetadata&
DawnSharedTextureCache::TextureMetadata::operator=(TextureMetadata&&) =
    default;

DawnSharedTextureCache::SharedTextureData::SharedTextureData() = default;
DawnSharedTextureCache::SharedTextureData::~SharedTextureData() = default;
DawnSharedTextureCache::SharedTextureData::SharedTextureData(
    SharedTextureData&&) = default;
DawnSharedTextureCache::SharedTextureData&
DawnSharedTextureCache::SharedTextureData::operator=(SharedTextureData&&) =
    default;

wgpu::SharedTextureMemory DawnSharedTextureCache::GetSharedTextureMemory(
    const wgpu::Device&) {
  return nullptr;
}
void DawnSharedTextureCache::MaybeCacheSharedTextureMemory(
    const wgpu::Device&,
    const wgpu::SharedTextureMemory&) {}
void DawnSharedTextureCache::EraseDawnSharedTextureCache(const wgpu::Device&) {
}
wgpu::Texture DawnSharedTextureCache::GetCachedWGPUTexture(
    const wgpu::Device&,
    wgpu::TextureUsage,
    wgpu::TextureUsage,
    const std::vector<wgpu::TextureFormat>&) {
  return nullptr;
}
void DawnSharedTextureCache::MaybeCacheWGPUTexture(
    const wgpu::Device&,
    const wgpu::Texture&,
    wgpu::TextureUsage,
    wgpu::TextureUsage,
    const std::vector<wgpu::TextureFormat>&) {}
void DawnSharedTextureCache::RemoveWGPUTextureFromCache(const wgpu::Device&,
                                                        const wgpu::Texture&) {
}
void DawnSharedTextureCache::DestroyWGPUTextureIfNotCached(
    const wgpu::Device&,
    const wgpu::Texture&) {}
void DawnSharedTextureCache::EraseDataIfDeviceLost() {}

DawnSharedTextureCache::TextureCache* DawnSharedTextureCache::GetTextureCache(
    const wgpu::Device&) {
  return nullptr;
}

// ---- DawnContextProvider (no instance can be created) ----------------------

wgpu::Device DawnContextProvider::GetDevice() const {
  base::ImmediateCrash();
}

// ---- SkiaGraphiteDawnImageRepresentation (never constructed) ---------------
// The dtor definition emits the vtable, so every virtual needs a body.

SkiaGraphiteDawnImageRepresentation::~SkiaGraphiteDawnImageRepresentation() {
  base::ImmediateCrash();
}
std::vector<sk_sp<SkSurface>>
SkiaGraphiteDawnImageRepresentation::BeginWriteAccess(const SkSurfaceProps&,
                                                      const gfx::Rect&) {
  base::ImmediateCrash();
}
std::vector<
    scoped_refptr<SkiaGraphiteDawnImageRepresentation::GraphiteTextureHolder>>
SkiaGraphiteDawnImageRepresentation::BeginWriteAccess() {
  base::ImmediateCrash();
}
void SkiaGraphiteDawnImageRepresentation::EndWriteAccess() {
  base::ImmediateCrash();
}
std::vector<
    scoped_refptr<SkiaGraphiteDawnImageRepresentation::GraphiteTextureHolder>>
SkiaGraphiteDawnImageRepresentation::BeginReadAccess() {
  base::ImmediateCrash();
}
void SkiaGraphiteDawnImageRepresentation::EndReadAccess() {
  base::ImmediateCrash();
}
wgpu::Device SkiaGraphiteDawnImageRepresentation::GetDevice() const {
  base::ImmediateCrash();
}
std::vector<
    scoped_refptr<SkiaGraphiteDawnImageRepresentation::GraphiteTextureHolder>>
SkiaGraphiteDawnImageRepresentation::WrapBackendTextures(
    wgpu::Texture,
    std::vector<skgpu::graphite::BackendTexture>) {
  base::ImmediateCrash();
}
std::vector<
    scoped_refptr<SkiaGraphiteDawnImageRepresentation::GraphiteTextureHolder>>
SkiaGraphiteDawnImageRepresentation::CreateBackendTextureHolders(
    wgpu::Texture,
    bool) {
  base::ImmediateCrash();
}

// ---- DawnEGLImageRepresentation (ProduceDawn-only path) --------------------

DawnEGLImageRepresentation::DawnEGLImageRepresentation(
    std::unique_ptr<GLTextureImageRepresentationBase> gl_representation,
    void*,
    SharedImageManager* manager,
    SharedImageBacking* backing,
    MemoryTypeTracker* tracker,
    const wgpu::Device&,
    std::vector<wgpu::TextureFormat>)
    : DawnImageRepresentation(manager, backing, tracker) {
  base::ImmediateCrash();
}
DawnEGLImageRepresentation::DawnEGLImageRepresentation(
    std::unique_ptr<GLTextureImageRepresentationBase> gl_representation,
    gl::ScopedEGLImage,
    SharedImageManager* manager,
    SharedImageBacking* backing,
    MemoryTypeTracker* tracker,
    const wgpu::Device&,
    std::vector<wgpu::TextureFormat>)
    : DawnImageRepresentation(manager, backing, tracker) {
  base::ImmediateCrash();
}
DawnEGLImageRepresentation::~DawnEGLImageRepresentation() {
  base::ImmediateCrash();
}
wgpu::Texture DawnEGLImageRepresentation::BeginAccess(wgpu::TextureUsage,
                                                      wgpu::TextureUsage) {
  base::ImmediateCrash();
}
void DawnEGLImageRepresentation::EndAccess() {
  base::ImmediateCrash();
}

}  // namespace gpu
