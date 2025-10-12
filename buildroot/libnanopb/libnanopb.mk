################################################################################
#
# libnanopb
#
################################################################################

LIBNANOPB_VERSION = 0.4.8
LIBNANOPB_SITE = https://github.com/nanopb/nanopb/archive
LIBNANOPB_SOURCE = $(LIBNANOPB_VERSION).tar.gz
LIBNANOPB_INSTALL_STAGING = YES
LIBNANOPB_LICENSE = Zlib
LIBNANOPB_LICENSE_FILES = LICENSE.txt

define LIBNANOPB_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -c $(@D)/pb_common.c -o $(@D)/pb_common.o
	$(TARGET_CC) $(TARGET_CFLAGS) -c $(@D)/pb_encode.c -o $(@D)/pb_encode.o
	$(TARGET_CC) $(TARGET_CFLAGS) -c $(@D)/pb_decode.c -o $(@D)/pb_decode.o
	$(TARGET_AR) rcs $(@D)/libnanopb.a $(@D)/pb_common.o $(@D)/pb_encode.o $(@D)/pb_decode.o
endef

define LIBNANOPB_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/libnanopb.a $(STAGING_DIR)/usr/lib/libnanopb.a
	$(INSTALL) -D -m 0644 $(@D)/pb.h $(STAGING_DIR)/usr/include/pb.h
	$(INSTALL) -D -m 0644 $(@D)/pb_common.h $(STAGING_DIR)/usr/include/pb_common.h
	$(INSTALL) -D -m 0644 $(@D)/pb_encode.h $(STAGING_DIR)/usr/include/pb_encode.h
	$(INSTALL) -D -m 0644 $(@D)/pb_decode.h $(STAGING_DIR)/usr/include/pb_decode.h
endef

$(eval $(generic-package))