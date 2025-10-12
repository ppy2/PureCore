################################################################################
#
# aprenderer
#
################################################################################

APRENDERER_SOURCE = aprenderer-arm32.tar.gz
APRENDERER_SITE = https://albumplayer.ru/linux
APRENDERER_DL_SUBDIR = aprenderer

define APRENDERER_EXTRACT_CMDS
    $(TAR) -xzf $(DL_DIR)/$(APRENDERER_DL_SUBDIR)/$(APRENDERER_SOURCE) -C $(@D)
endef

define APRENDERER_INSTALL_TARGET_CMDS
    $(INSTALL) -d -m 0755 $(TARGET_DIR)/usr
    $(TAR) -xzf $(DL_DIR)/$(APRENDERER_DL_SUBDIR)/$(APRENDERER_SOURCE) -C $(TARGET_DIR)/usr
endef

$(eval $(generic-package))
