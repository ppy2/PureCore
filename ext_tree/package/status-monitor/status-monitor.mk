################################################################################
#
# status-monitor
#
################################################################################

STATUS_MONITOR_VERSION = 1.0
STATUS_MONITOR_SITE_METHOD = local
STATUS_MONITOR_SITE = $(TOPDIR)/../ext_tree/package/status-monitor/src
STATUS_MONITOR_LICENSE = GPL-2.0+

# Dependencies (alsa-lib for ALSA API, dbus for D-Bus)
STATUS_MONITOR_DEPENDENCIES = alsa-lib dbus

define STATUS_MONITOR_BUILD_CMDS
    cd $(@D) && \
    $(TARGET_CONFIGURE_OPTS) \
    $(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
	-Wall -O2 -s \
	-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
	`$(PKG_CONFIG_HOST_BINARY) --cflags dbus-1` \
	-o status_monitor \
	status_monitor_dbus.c \
	-lasound `$(PKG_CONFIG_HOST_BINARY) --libs dbus-1`
	
    cd $(@D) && \
    $(TARGET_CONFIGURE_OPTS) \
    $(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
	-Wall -O2 -s \
	-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
	`$(PKG_CONFIG_HOST_BINARY) --cflags dbus-1` \
	-o dbus_notify \
	dbus_notify.c \
	`$(PKG_CONFIG_HOST_BINARY) --libs dbus-1`
endef

define STATUS_MONITOR_INSTALL_TARGET_CMDS
    $(INSTALL) -d $(TARGET_DIR)/opt
    $(INSTALL) -m 755 $(@D)/status_monitor $(TARGET_DIR)/opt/
    $(INSTALL) -m 755 $(@D)/dbus_notify $(TARGET_DIR)/opt/
    $(INSTALL) -d $(TARGET_DIR)/etc/init.d
    $(INSTALL) -m 755 $(@D)/S01statusmonitor $(TARGET_DIR)/etc/init.d/
endef

$(eval $(generic-package))