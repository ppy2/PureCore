################################################################################
#
# Buildroot package: librespot
#
################################################################################

#LIBRESPOT_VERSION = $(call qstrip,$(or $(BR2_PACKAGE_LIBRESPOT_VERSION),v0.7.1))
LIBRESPOT_VERSION = $(call qstrip,$(or $(BR2_PACKAGE_LIBRESPOT_VERSION),84a3302168b8b25d44f1b313ca20155959f553e0))
LIBRESPOT_SITE = https://github.com/librespot-org/librespot.git
LIBRESPOT_SITE_METHOD = git

LIBRESPOT_LICENSE = MIT
LIBRESPOT_CARGO_BUILD_OPTS = $(call qstrip,$(BR2_PACKAGE_LIBRESPOT_BUILD_OPTS))
LIBRESPOT_DEPENDENCIES += host-rust-bindgen

LIBRESPOT_DEPENDENCIES = alsa-lib  host-rust-bindgen host-pkgconf

define LIBRESPOT_FIX_CPAL_CHECKSUM
	sed -i 's/"src\/host\/alsa\/mod.rs":"8e7cc24a805b4729d43e01c1f0a7d315b1cb7b80fd97e394326a3c06cbf0eea9"/"src\/host\/alsa\/mod.rs":"b88a2b1d5e49dd4fecc158e11dbc484bbd6b554a8347dfdfd65a170bd2d46a57"/g' \
		$(@D)/VENDOR/cpal/.cargo-checksum.json
endef
LIBRESPOT_POST_PATCH_HOOKS += LIBRESPOT_FIX_CPAL_CHECKSUM

$(eval $(cargo-package))
