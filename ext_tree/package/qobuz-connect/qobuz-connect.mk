################################################################################
#
# qobuz-connect
#
################################################################################

QOBUZ_CONNECT_VERSION = main
QOBUZ_CONNECT_SITE = https://github.com/ppy2/qobuz-connect-purefox.git
QOBUZ_CONNECT_SITE_METHOD = git
QOBUZ_CONNECT_LICENSE = Proprietary
QOBUZ_CONNECT_DEPENDENCIES = alsa-lib avahi civetweb openssl

define QOBUZ_CONNECT_BUILD_CMDS
	# Create civetweb library manually since buildroot doesn't install it to staging
	cd $(BUILD_DIR)/civetweb-1.16 && $(TARGET_MAKE_ENV) $(MAKE) CC="$(TARGET_CC)" AR="$(TARGET_AR)" lib
	$(INSTALL) -D -m 644 $(BUILD_DIR)/civetweb-1.16/libcivetweb.a $(STAGING_DIR)/usr/lib/
	$(INSTALL) -D -m 644 $(BUILD_DIR)/civetweb-1.16/include/civetweb.h $(STAGING_DIR)/usr/include/
	
	mkdir -p $(@D)/build
	cd $(@D)/build && \
	PKG_CONFIG_PATH=$(STAGING_DIR)/usr/lib/pkgconfig:$(STAGING_DIR)/usr/share/pkgconfig \
	/usr/bin/cmake \
		-DCMAKE_TOOLCHAIN_FILE=$(HOST_DIR)/share/buildroot/toolchainfile.cmake \
		-DCMAKE_INSTALL_PREFIX=/usr \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_FLAGS="-O2 -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable -Wno-format" \
		-DCMAKE_PREFIX_PATH=$(STAGING_DIR)/usr \
		-DCIVETWEB_LIB="$(STAGING_DIR)/usr/lib/libcivetweb.a;-lz" \
		-DCIVETWEB_INCLUDE_DIR=$(STAGING_DIR)/usr/include \
		../sample_console_app && \
	$(MAKE)
endef

define QOBUZ_CONNECT_INSTALL_TARGET_CMDS
	$(INSTALL) -d $(TARGET_DIR)/opt/qobuz-connect
	$(INSTALL) -m 755 $(@D)/build/qobuz_connect_sample_app $(TARGET_DIR)/opt/qobuz-connect/qobuz-connect
	$(INSTALL) -d $(TARGET_DIR)/usr/lib
	$(INSTALL) -m 755 $(@D)/sdk/lib/libqobuz_connect.so.1.0.0 $(TARGET_DIR)/usr/lib/
	ln -sf libqobuz_connect.so.1.0.0 $(TARGET_DIR)/usr/lib/libqobuz_connect.so
	$(INSTALL) -m 755 $(@D)/third_party/libcjson/lib/libcjson.so.1.7.13 $(TARGET_DIR)/usr/lib/
	ln -sf libcjson.so.1.7.13 $(TARGET_DIR)/usr/lib/libcjson.so.1
	ln -sf libcjson.so.1 $(TARGET_DIR)/usr/lib/libcjson.so
	$(INSTALL) -m 755 $(@D)/third_party/libuv/lib/libuv.so.1.0.0 $(TARGET_DIR)/usr/lib/
	ln -sf libuv.so.1.0.0 $(TARGET_DIR)/usr/lib/libuv.so.1
	ln -sf libuv.so.1 $(TARGET_DIR)/usr/lib/libuv.so
endef

$(eval $(generic-package))