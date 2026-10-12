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

# Framework config overlay (desktop mode on the internal display)
PRODUCT_PACKAGES += ours_overlay_desktop
PRODUCT_PACKAGES += OursLawnchair

# ARM64 -> x86_64 NativeBridge (armtr). Opt-in: needs external/unicorn + external/armtr
# (scripts/sync-translator-to-aosp.sh). Usage: ARMTR_ENABLE=true m ...
ARMTR_ENABLE ?= false
ifeq ($(ARMTR_ENABLE),true)
PRODUCT_PACKAGES += libarmtr_bridge
endif
