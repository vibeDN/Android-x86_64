/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Android-x86_64 project
 *
 * NativeBridge shim (callbacks v8) for an ARM64 guest on an x86_64 device.
 * Milestone M0: every callback logs and returns a conservative answer; no
 * guest code is run. See docs/translator/02-native-bridge-design.md.
 */
#define LOG_TAG "OursNB"

#include <log/log.h>
#include <string.h>

#include "nativebridge/native_bridge.h"

#include "engine.h"

// Layout is private to ART (art/libnativebridge/native_bridge.cc) and not in the public header.
namespace android {
struct NativeBridgeRuntimeValues {
  const char* os_arch;
  const char* cpu_abi;
  const char* cpu_abi2;
  const char* const* supported_abis;
  int32_t abi_count;
};
}  // namespace android

namespace {

constexpr uint32_t kMinVersion = 2;  // v2 is needed so ART calls getSignalHandler.
constexpr uint32_t kVersion = 8;     // isNativeBridgeFunctionPointer.
constexpr const char* kGuestIsa = "arm64";
// Matches <app>/lib/arm64/x.so and <app>/base.apk!/lib/arm64-v8a/x.so
constexpr const char* kSupportedPathSubstring = "/lib/arm64";

const android::NativeBridgeRuntimeCallbacks* g_runtime = nullptr;
const char* g_error = "not implemented";

// Must be non-null or ART disables the bridge. Fields other than os_arch are deprecated.
const android::NativeBridgeRuntimeValues kRuntimeValues = {
    .os_arch = "aarch64",
    .cpu_abi = "arm64-v8a",
    .cpu_abi2 = nullptr,
    .supported_abis = nullptr,
    .abi_count = 0,
};

bool nb_initialize(const android::NativeBridgeRuntimeCallbacks* cbs, const char* private_dir,
                   const char* isa) {
  ALOGI("initialize(private_dir=%s, isa=%s, engine=%s)", private_dir ? private_dir : "(null)",
        isa ? isa : "(null)", ours_engine_name());
  if (isa == nullptr || strcmp(isa, kGuestIsa) != 0) return false;
  g_runtime = cbs;
  // TODO(M1): ours_engine_init(hooks). TODO(M2): start guest linker64/app_process64 (see design 2.).
  return true;
}

void* nb_loadLibrary(const char* path, int flag) {
  ALOGI("loadLibrary(%s, 0x%x) -> not implemented", path ? path : "(null)", flag);
  // TODO(M3): guest dlopen_ext, then fallback to host android_dlopen_ext.
  return nullptr;
}

void* nb_getTrampoline(void* handle, const char* name, const char* shorty, uint32_t len) {
  ALOGI("getTrampoline(%p, %s, %s, %u)", handle, name ? name : "", shorty ? shorty : "", len);
  return nullptr;  // TODO(M3)
}

void* nb_getTrampolineWithJNICallType(void* handle, const char* name, const char* shorty,
                                      uint32_t len, enum android::JNICallType type) {
  ALOGI("getTrampolineWithJNICallType(%p, %s, %s, %u, %d)", handle, name ? name : "",
        shorty ? shorty : "", len, static_cast<int>(type));
  return nullptr;  // TODO(M3): DlSym + WrapGuestJNIFunction / JNI_OnLoad wrapper.
}

void* nb_getTrampolineForFunctionPointer(const void* method, const char* shorty, uint32_t len,
                                         enum android::JNICallType type) {
  ALOGI("getTrampolineForFunctionPointer(%p, %s, %u, %d)", method, shorty ? shorty : "", len,
        static_cast<int>(type));
  return nullptr;  // TODO(M3): RegisterNatives path.
}

bool nb_isNativeBridgeFunctionPointer(const void* method) {
  ALOGI("isNativeBridgeFunctionPointer(%p)", method);
  return false;  // TODO(M3): true iff inside a guest-executable mapping.
}

bool nb_isSupported(const char* path) {
  ALOGI("isSupported(%s)", path ? path : "(null)");
  return true;  // Same as Berberis: path filtering is done by isPathSupported.
}

bool nb_isPathSupported(const char* path) {
  bool r = path != nullptr && strstr(path, kSupportedPathSubstring) != nullptr;
  ALOGI("isPathSupported(%s) -> %d", path ? path : "(null)", r);
  return r;
}

const android::NativeBridgeRuntimeValues* nb_getAppEnv(const char* isa) {
  ALOGI("getAppEnv(%s)", isa ? isa : "(null)");
  return (isa && strcmp(isa, kGuestIsa) == 0) ? &kRuntimeValues : nullptr;
}

bool nb_isCompatibleWith(uint32_t v) {
  ALOGI("isCompatibleWith(%u)", v);
  return v >= kMinVersion && v <= kVersion;
}

android::NativeBridgeSignalHandlerFn nb_getSignalHandler(int sig) {
  ALOGI("getSignalHandler(%d)", sig);
  return nullptr;  // Guest signals are emulated by the bridge itself (design 5.4).
}

int nb_unloadLibrary(void* handle) {
  ALOGI("unloadLibrary(%p)", handle);
  return 0;  // Like Berberis: leak.
}

const char* nb_getError() { return g_error; }

bool nb_initAnonymousNamespace(const char* sonames, const char* path) {
  ALOGI("initAnonymousNamespace(%s, %s)", sonames ? sonames : "", path ? path : "");
  return true;  // TODO(M3)
}

android::native_bridge_namespace_t* nb_createNamespace(const char* name, const char* ld_path,
                                                       const char* default_path, uint64_t type,
                                                       const char* permitted,
                                                       android::native_bridge_namespace_t* parent) {
  ALOGI("createNamespace(%s, ld=%s, default=%s, type=%llu, permitted=%s, parent=%p)",
        name ? name : "", ld_path ? ld_path : "", default_path ? default_path : "",
        static_cast<unsigned long long>(type), permitted ? permitted : "", parent);
  return nullptr;  // TODO(M3): {guest ns, host ns} pair like Berberis NdktNativeBridge.
}

bool nb_linkNamespaces(android::native_bridge_namespace_t* from,
                       android::native_bridge_namespace_t* to, const char* sonames) {
  ALOGI("linkNamespaces(%p, %p, %s)", from, to, sonames ? sonames : "");
  return true;
}

void* nb_loadLibraryExt(const char* path, int flag, android::native_bridge_namespace_t* ns) {
  ALOGI("loadLibraryExt(%s, 0x%x, ns=%p) -> not implemented", path ? path : "(null)", flag, ns);
  return nullptr;
}

android::native_bridge_namespace_t* nb_getVendorNamespace() {
  ALOGI("getVendorNamespace()");
  return nullptr;
}

android::native_bridge_namespace_t* nb_getExportedNamespace(const char* name) {
  ALOGI("getExportedNamespace(%s)", name ? name : "");
  return nullptr;
}

void nb_preZygoteFork() { ALOGI("preZygoteFork()"); }

}  // namespace

extern "C" {
// ART looks up this symbol in the library named by ro.dalvik.vm.native.bridge.
android::NativeBridgeCallbacks NativeBridgeItf = {
    kVersion,
    &nb_initialize,
    &nb_loadLibrary,
    &nb_getTrampoline,
    &nb_isSupported,
    &nb_getAppEnv,
    &nb_isCompatibleWith,
    &nb_getSignalHandler,
    &nb_unloadLibrary,
    &nb_getError,
    &nb_isPathSupported,
    &nb_initAnonymousNamespace,
    &nb_createNamespace,
    &nb_linkNamespaces,
    &nb_loadLibraryExt,
    &nb_getVendorNamespace,
    &nb_getExportedNamespace,
    &nb_preZygoteFork,
    &nb_getTrampolineWithJNICallType,
    &nb_getTrampolineForFunctionPointer,
    &nb_isNativeBridgeFunctionPointer,
};
}
