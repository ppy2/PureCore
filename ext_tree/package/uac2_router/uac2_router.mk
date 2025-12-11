################################################################################
#
# uac2_router
#
################################################################################

UAC2_ROUTER_VERSION = 2.3
UAC2_ROUTER_SITE = $(TOPDIR)/../ext_tree/package/uac2_router/src
UAC2_ROUTER_SITE_METHOD = local
UAC2_ROUTER_LICENSE = MIT
UAC2_ROUTER_LICENSE_FILES = LICENSE
UAC2_ROUTER_DEPENDENCIES = alsa-lib

define UAC2_ROUTER_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -o $(@D)/uac2_router $(@D)/uac2_router.c $(TARGET_LDFLAGS) -lasound -lpthread
endef

define UAC2_ROUTER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/uac2_router $(TARGET_DIR)/usr/bin/uac2_router
	$(INSTALL) -D -m 0755 $(@D)/S99uac2_router $(TARGET_DIR)/etc/init.d/S99uac2_router
endef

$(eval $(generic-package))
