################################################################################
# thttpd
################################################################################

THTTPD_VERSION = 2.29
THTTPD_SOURCE = thttpd-$(THTTPD_VERSION).tar.gz
THTTPD_SITE = http://www.acme.com/software/thttpd
THTTPD_LICENSE = BSD-3-Clause
THTTPD_LICENSE_FILES = LICENSE

define THTTPD_CONFIGURE_CMDS
    @$(call MESSAGE,"Creating custom Makefile and fixing sources")

    # === Создаём Makefile ===
    echo 'SRCS = thttpd.c match.c htpasswd.c libhttpd.c libdecode64.c libencode64.c libmime.c libsha1.c' > $(@D)/Makefile
    echo 'OBJS = $$(SRCS:.c=.o)' >> $(@D)/Makefile
    echo 'CC = $(TARGET_CC)' >> $(@D)/Makefile
    echo 'CFLAGS = $(TARGET_CFLAGS) -I. -D_REENTRANT -DHAS_DIRENT_NAMLEN -DHAS_SENDFILE -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64' >> $(@D)/Makefile
    echo 'LDFLAGS = $(TARGET_LDFLAGS)' >> $(@D)/Makefile
    echo 'LIBS = -lpthread' >> $(@D)/Makefile
    echo '' >> $(@D)/Makefile
    echo 'all: thttpd htpasswd' >> $(@D)/Makefile
    echo '' >> $(@D)/Makefile
    echo 'thttpd: $$(filter-out htpasswd.o,$$(OBJS))' >> $(@D)/Makefile
    echo '	$$(CC) $$(LDFLAGS) -o $$@ $$^ $$(LIBS)' >> $(@D)/Makefile
    echo '' >> $(@D)/Makefile
    echo 'htpasswd: htpasswd.o libsha1.o' >> $(@D)/Makefile
    echo '	$$(CC) $$(LDFLAGS) -o $$@ $$^ $$(LIBS)' >> $(@D)/Makefile
    echo '' >> $(@D)/Makefile
    echo '%.o: %.c' >> $(@D)/Makefile
    echo '	$$(CC) $$(CFLAGS) -c $$< -o $$@' >> $(@D)/Makefile
    echo '' >> $(@D)/Makefile
    echo 'clean:' >> $(@D)/Makefile
    echo '	rm -f $$(OBJS) thttpd htpasswd *~' >> $(@D)/Makefile
    echo '.PHONY: all clean' >> $(@D)/Makefile

    # === Исправляем thttpd.c ===
    # Добавляем нужные заголовки
    $(SED) '/#include <sys\/socket\.h>/a\#include <fcntl.h>\n#include <time.h>\n#include <grp.h>' $(@D)/thttpd.c
    # Чиним fcntl вместо setsockopt
    $(SED) 's/setsockopt( fileno( logfp ), F_SETFD, 1 );/fcntl( fileno( logfp ), F_SETFD, 1 );/g' $(@D)/thttpd.c
    $(SED) 's/setsockopt( fileno( logfp ), F_SETFD, 1 );/fcntl( fileno( logfp ), F_SETFD, 1 );/g' $(@D)/thttpd.c
    # Убираем лишнее переопределение socklen_t
    $(SED) '/typedef int socklen_t;/d' $(@D)/libhttpd.c
    # Убираем предупреждения
    $(SED) 's/(void) tzset();//g' $(@D)/thttpd.c

    # === Создаём mime_encodings.h если нет ===
    if [ ! -f $(@D)/mime_encodings.h ]; then \
	echo "Creating dummy mime_encodings.h"; \
	echo '#ifndef MIME_ENCODINGS_H' > $(@D)/mime_encodings.h; \
	echo '#define MIME_ENCODINGS_H' >> $(@D)/mime_encodings.h; \
	echo 'extern const char *encodingtypes[];' >> $(@D)/mime_encodings.h; \
	echo 'extern const char *encodingsuffixes[];' >> $(@D)/mime_encodings.h; \
	echo '#endif' >> $(@D)/mime_encodings.h; \
	echo 'const char *encodingtypes[] = { NULL };' > $(@D)/mime_encodings.c; \
	echo 'const char *encodingsuffixes[] = { NULL };' >> $(@D)/mime_encodings.c; \
	echo 'SRCS += mime_encodings.c' >> $(@D)/Makefile; \
    fi
endef

# Сборка
define THTTPD_BUILD_CMDS
    $(MAKE) -C $(@D)
endef

# Установка
define THTTPD_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 755 $(@D)/thttpd $(TARGET_DIR)/usr/sbin/thttpd
    $(INSTALL) -D -m 755 $(@D)/htpasswd $(TARGET_DIR)/usr/bin/htpasswd
endef

# Init скрипты
define THTTPD_INSTALL_INIT_SYSV
    $(INSTALL) -D -m 755 $(@D)/S90thttpd $(TARGET_DIR)/etc/init.d/S90thttpd
endef

define THTTPD_INSTALL_INIT_SYSTEMD
    $(INSTALL) -D -m 644 $(@D)/thttpd.service $(TARGET_DIR)/usr/lib/systemd/system/thttpd.service
endef

$(eval $(generic-package))
