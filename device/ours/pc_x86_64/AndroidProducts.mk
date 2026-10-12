# Own product: Android desktop OS for x86_64 PCs (built on the Cuttlefish phone product for now).

PRODUCT_MAKEFILES := \
    ours_cf_x86_64_phone:$(LOCAL_DIR)/ours_cf_x86_64_phone.mk

COMMON_LUNCH_CHOICES := \
    ours_cf_x86_64_phone-trunk_staging-userdebug
