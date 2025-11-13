################################################################################
#
# uac2_router
#
################################################################################

UAC2_ROUTER_VERSION = 1.0
UAC2_ROUTER_SITE_METHOD = local
UAC2_ROUTER_SITE = $(TOPDIR)/../ext_tree/package/uac2_router
UAC2_ROUTER_DEPENDENCIES = alsa-lib

define UAC2_ROUTER_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/uac2_router \
		$(@D)/uac2_router.c \
		-lasound -lpthread
endef

define UAC2_ROUTER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/uac2_router $(TARGET_DIR)/opt/uac2_router
endef

$(eval $(generic-package))
