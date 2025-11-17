################################################################################
#
# wasapi_router
#
################################################################################

WASAPI_ROUTER_VERSION = 1.0
WASAPI_ROUTER_SITE_METHOD = local
WASAPI_ROUTER_SITE = /opt/PureFox/ext_tree/package/wasapi-router/src

WASAPI_ROUTER_LICENSE = GPL-2.0+
WASAPI_ROUTER_LICENSE_FILES = COPYING

WASAPI_ROUTER_DEPENDENCIES = alsa-lib

define WASAPI_ROUTER_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/wasapi_router $(@D)/wasapi_router.c \
		-lasound -lpthread
endef

define WASAPI_ROUTER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/wasapi_router $(TARGET_DIR)/opt/wasapi_router
endef

$(eval $(generic-package))