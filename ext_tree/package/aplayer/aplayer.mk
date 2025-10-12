################################################################################
#
# aplayer
#
################################################################################

APLAYER_SOURCE = aplayer-arm32.tar.gz
APLAYER_SITE = https://albumplayer.ru/linux
APLAYER_DL_SUBDIR = aplayer

define APLAYER_EXTRACT_CMDS
    $(TAR) -xzf $(DL_DIR)/$(APLAYER_DL_SUBDIR)/$(APLAYER_SOURCE) -C $(@D)
endef

define APLAYER_INSTALL_TARGET_CMDS
    $(INSTALL) -d -m 0755 $(TARGET_DIR)/usr
    $(TAR) -xzf $(DL_DIR)/$(APLAYER_DL_SUBDIR)/$(APLAYER_SOURCE) -C $(TARGET_DIR)/usr
endef

$(eval $(generic-package))
