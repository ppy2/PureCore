################################################################################
#
# simple_router
#
################################################################################

SIMPLE_ROUTER_VERSION = 1.0
SIMPLE_ROUTER_SITE_METHOD = local
SIMPLE_ROUTER_SITE = /opt/PureFox/ext_tree/package/simple-router/src

SIMPLE_ROUTER_LICENSE = GPL-2.0+
SIMPLE_ROUTER_LICENSE_FILES = COPYING

SIMPLE_ROUTER_DEPENDENCIES = alsa-lib

define SIMPLE_ROUTER_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/simple_router $(@D)/simple_router.c \
		-lasound -lpthread
endef

define SIMPLE_ROUTER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/simple_router $(TARGET_DIR)/opt/simple_router
endef

$(eval $(generic-package))