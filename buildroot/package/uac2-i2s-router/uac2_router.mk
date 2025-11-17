UAC2_I2S_ROUTER_VERSION = 1.0
UAC2_I2S_ROUTER_SITE = $(BR2_EXTERNAL_PUREFOX_PATH)/package/uac2-i2s-router
UAC2_I2S_ROUTER_SITE_METHOD = local
UAC2_I2S_ROUTER_DEPENDENCIES = alsa-lib

define UAC2_I2S_ROUTER_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/uac2_router.c -o $(@D)/uac2_router \
		-lasound -lpthread
endef

define UAC2_I2S_ROUTER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/uac2_router $(TARGET_DIR)/usr/bin/uac2_router
	$(INSTALL) -D -m 0755 $(@D)/S99uac2router $(TARGET_DIR)/etc/init.d/S99uac2router
endef

$(eval $(generic-package))
