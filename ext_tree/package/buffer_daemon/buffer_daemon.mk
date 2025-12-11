################################################################################
#
# buffer_daemon
#
################################################################################

BUFFER_DAEMON_VERSION = 1.0
BUFFER_DAEMON_SITE = $(TOPDIR)/../ext_tree/package/buffer_daemon/src
BUFFER_DAEMON_SITE_METHOD = local
BUFFER_DAEMON_LICENSE = MIT
BUFFER_DAEMON_LICENSE_FILES = LICENSE
BUFFER_DAEMON_DEPENDENCIES = alsa-lib

define BUFFER_DAEMON_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -o $(@D)/buffer_daemon $(@D)/buffer_daemon.c $(TARGET_LDFLAGS) -lasound
endef

define BUFFER_DAEMON_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/buffer_daemon $(TARGET_DIR)/usr/bin/buffer_daemon
	$(INSTALL) -D -m 0755 $(@D)/S99buffer_daemon $(TARGET_DIR)/etc/init.d/S99buffer_daemon
endef

$(eval $(generic-package))