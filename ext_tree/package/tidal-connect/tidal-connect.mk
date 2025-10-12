TIDAL_CONNECT_VERSION = 1.0
TIDAL_CONNECT_SITE = $(TOPDIR)/../ext_tree/package/tidal-connect/files
TIDAL_CONNECT_SITE_METHOD = local

define TIDAL_CONNECT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/tidalconnect $(TARGET_DIR)/sbin/tidalconnect
	$(INSTALL) -D -m 0755 $(@D)/S95tidal $(TARGET_DIR)/etc/rc.pure/S95tidal
	mkdir -p $(TARGET_DIR)/usr/lib/tidal
	cp -a $(@D)/lib/*.so* $(TARGET_DIR)/usr/lib/tidal/
endef

$(eval $(generic-package))
