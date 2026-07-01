// mb_dawn_stubs.cc — trap-stub definitions for the handful of Dawn (WebGPU) symbols that
// KEPT Chromium code (gpu/gles2 + shared-image) references UNGATED even when use_dawn=false.
//
// Compiled ONLY when WebGPU is off (BUILD.gn adds this file in the `if (!use_dawn)` branch;
// with --webgpu, Dawn defines these for real and this file is absent). With WebGPU off no
// Dawn adapter is ever created, so these are never CALLED — but they must be DEFINED, or dyld
// aborts at load ("symbol not found in flat namespace") when GPU init binds the reference.
// (A -Wl,-U merely allows them undefined, which just defers that failure to load time and
// crashes deterministically under the ThinLTO/lld --size-optimized link.)
//
// Each symbol is aliased to one trap stub. If a WebGPU-off build somehow reaches one, it
// traps loudly rather than corrupting state. Names are pinned to this Chromium; regenerate on
// an uprev: relink with -undefined dynamic_lookup, then `nm -u | grep -iE 'Dawn|wgpu|dawn'`.

extern "C" void mb_dawn_stub_trap() { __builtin_trap(); }

asm(
    ".globl __ZN3gpu22DawnSharedTextureCache20GetCachedWGPUTextureERKN4wgpu6DeviceENS1_12TextureUsageES5_RKNSt4__Cr6vectorINS1_13TextureFormatENS6_9allocatorIS8_EEEE\n"
    ".set __ZN3gpu22DawnSharedTextureCache20GetCachedWGPUTextureERKN4wgpu6DeviceENS1_12TextureUsageES5_RKNSt4__Cr6vectorINS1_13TextureFormatENS6_9allocatorIS8_EEEE, _mb_dawn_stub_trap\n"
    ".globl __ZN3gpu22DawnSharedTextureCache21EraseDataIfDeviceLostEv\n"
    ".set __ZN3gpu22DawnSharedTextureCache21EraseDataIfDeviceLostEv, _mb_dawn_stub_trap\n"
    ".globl __ZN3gpu22DawnSharedTextureCache21MaybeCacheWGPUTextureERKN4wgpu6DeviceERKNS1_7TextureENS1_12TextureUsageES8_RKNSt4__Cr6vectorINS1_13TextureFormatENS9_9allocatorISB_EEEE\n"
    ".set __ZN3gpu22DawnSharedTextureCache21MaybeCacheWGPUTextureERKN4wgpu6DeviceERKNS1_7TextureENS1_12TextureUsageES8_RKNSt4__Cr6vectorINS1_13TextureFormatENS9_9allocatorISB_EEEE, _mb_dawn_stub_trap\n"
    ".globl __ZN3gpu22DawnSharedTextureCache22GetSharedTextureMemoryERKN4wgpu6DeviceE\n"
    ".set __ZN3gpu22DawnSharedTextureCache22GetSharedTextureMemoryERKN4wgpu6DeviceE, _mb_dawn_stub_trap\n"
    ".globl __ZN3gpu22DawnSharedTextureCache26RemoveWGPUTextureFromCacheERKN4wgpu6DeviceERKNS1_7TextureE\n"
    ".set __ZN3gpu22DawnSharedTextureCache26RemoveWGPUTextureFromCacheERKN4wgpu6DeviceERKNS1_7TextureE, _mb_dawn_stub_trap\n"
    ".globl __ZN3gpu22DawnSharedTextureCache29DestroyWGPUTextureIfNotCachedERKN4wgpu6DeviceERKNS1_7TextureE\n"
    ".set __ZN3gpu22DawnSharedTextureCache29DestroyWGPUTextureIfNotCachedERKN4wgpu6DeviceERKNS1_7TextureE, _mb_dawn_stub_trap\n"
    ".globl __ZN3gpu22DawnSharedTextureCache29MaybeCacheSharedTextureMemoryERKN4wgpu6DeviceERKNS1_19SharedTextureMemoryE\n"
    ".set __ZN3gpu22DawnSharedTextureCache29MaybeCacheSharedTextureMemoryERKN4wgpu6DeviceERKNS1_19SharedTextureMemoryE, _mb_dawn_stub_trap\n"
    ".globl __ZN3gpu22DawnSharedTextureCacheC1Ev\n"
    ".set __ZN3gpu22DawnSharedTextureCacheC1Ev, _mb_dawn_stub_trap\n"
    ".globl __ZN3gpu22DawnSharedTextureCacheD1Ev\n"
    ".set __ZN3gpu22DawnSharedTextureCacheD1Ev, _mb_dawn_stub_trap\n"
    ".globl __ZN3gpu31DawnFallbackImageRepresentationC1EPNS_18SharedImageManagerEPNS_18SharedImageBackingEPNS_17MemoryTypeTrackerEN4wgpu6DeviceENS7_13TextureFormatENSt4__Cr6vectorIS9_NSA_9allocatorIS9_EEEE\n"
    ".set __ZN3gpu31DawnFallbackImageRepresentationC1EPNS_18SharedImageManagerEPNS_18SharedImageBackingEPNS_17MemoryTypeTrackerEN4wgpu6DeviceENS7_13TextureFormatENSt4__Cr6vectorIS9_NSA_9allocatorIS9_EEEE, _mb_dawn_stub_trap\n"
    ".globl __ZN4dawn6native5metal12GetMTLDeviceEP14WGPUDeviceImpl\n"
    ".set __ZN4dawn6native5metal12GetMTLDeviceEP14WGPUDeviceImpl, _mb_dawn_stub_trap\n"
    ".globl __ZNK3gpu19DawnContextProvider9GetDeviceEv\n"
    ".set __ZNK3gpu19DawnContextProvider9GetDeviceEv, _mb_dawn_stub_trap\n"
);
