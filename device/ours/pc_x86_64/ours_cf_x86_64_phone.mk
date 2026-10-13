# Android desktop OS (x86_64). Reuses the Cuttlefish phone product and turns on desktop windowing.
$(call inherit-product, device/google/cuttlefish/vsoc_x86_64/phone/aosp_cf.mk)

PRODUCT_NAME := ours_cf_x86_64_phone
PRODUCT_MODEL := Android Desktop PC

# Desktop windowing: run the desktop experience without the developer-option toggle and without
# the device whitelist. Needed on non-certified hardware.
PRODUCT_PRODUCT_PROPERTIES += \
    persist.wm.debug.desktop_experience_devopts=true \
    persist.wm.debug.desktop_mode_enforce_device_restrictions=false \
    persist.wm.debug.enter_desktop_by_default_on_freeform_display=true

# PC form factor + freeform windows
PRODUCT_COPY_FILES += \
    device/ours/pc_x86_64/android.hardware.type.pc.xml:$(TARGET_COPY_OUT_PRODUCT)/etc/permissions/android.hardware.type.pc.xml \
    frameworks/native/data/etc/android.software.freeform_window_management.xml:$(TARGET_COPY_OUT_PRODUCT)/etc/permissions/android.software.freeform_window_management.xml

# Default windowing mode of the display: freeform
PRODUCT_COPY_FILES += \
    device/ours/pc_x86_64/display_settings.xml:$(TARGET_COPY_OUT_VENDOR)/etc/display_settings.xml

# Networking: keep the first NIC as eth0 so EthernetService (regex (eth|usb)\d+, feature
# android.hardware.ethernet from cuttlefish device.mk) manages it with DHCP from NetworkStack. Cuttlefish
# renames it to buried_eth0 (cellular backing for its RIL), which no PC needs.
PRODUCT_VENDOR_PROPERTIES += ro.vendor.disable_rename_eth0=true

# Framework config overlay (desktop mode on the internal display)
PRODUCT_PACKAGES += ours_overlay_desktop
PRODUCT_PACKAGES += OursLawnchair

# ARM64 -> x86_64 NativeBridge (armtr), docs/translator/05-native-bridge.md. Needs external/unicorn + external/armtr +
# device/ours/pc_x86_64/translator/guest (scripts/sync-translator-to-aosp.sh). Disable with ARMTR_ENABLE=false.
# cuttlefish's vsoc_x86_64 BoardConfig already sets TARGET_NATIVE_BRIDGE_ABI := arm64-v8a, so
# ro.product.cpu.abilist64 = x86_64,arm64-v8a.
ARMTR_ENABLE ?= true
ifeq ($(ARMTR_ENABLE),true)
PRODUCT_PACKAGES += libarmtr_bridge armtr_guest_root
# Overrides `ro.dalvik.vm.native.bridge?=0` of build/make/target/product/runtime_libart.mk (like Berberis'
# enable_riscv64_to_x86_64.mk). isa.arm64=x86_64 marks arm64 as emulated for PackageManager/ART (dex2oat isa).
# ro.enable.native.bridge.exec is not set: no binfmt_misc runner for arm64 executables (apps only).
PRODUCT_SYSTEM_PROPERTIES += \
    ro.dalvik.vm.native.bridge=libarmtr_bridge.so \
    ro.dalvik.vm.isa.arm64=x86_64
# generic_system.mk's artifact path requirement forbids device-added system props unless allow-listed.
PRODUCT_ARTIFACT_PATH_REQUIREMENT_SYSPROP_ALLOWED_LIST += ro.dalvik.vm.native.bridge ro.dalvik.vm.isa.arm64
PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST += \
    system/lib64/libarmtr_bridge.so system/bin/armtr/% system/lib64/armtr/% system/etc/ld.config.arm64.txt
endif
