################################################################################
#
# toolchain-external-arm-arm
#
################################################################################

TOOLCHAIN_EXTERNAL_ARM_ARM_VERSION = 14.2.rel1
TOOLCHAIN_EXTERNAL_ARM_ARM_SITE = https://github.com/DLTcollab/toolchain-arm/raw/refs/heads/main

TOOLCHAIN_EXTERNAL_ARM_ARM_SOURCE = arm-gnu-toolchain-$(TOOLCHAIN_EXTERNAL_ARM_ARM_VERSION)-$(HOSTARCH)-arm-none-linux-gnueabihf.tar.xz

$(eval $(toolchain-external-package))
