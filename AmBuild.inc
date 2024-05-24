##
## linux/AmBuild.inc
##
## Copyright 2022 Ambarella Inc.  All Rights Reserved.
##
## This program is free software; you can redistribute it and/or
## modify it under the terms of the GNU General Public License
## as published by the Free Software Foundation; either version 2
## of the License, or (at your option) any later version.
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##

ifeq ($(CONFIG_LINUX_KERNEL_VERSION),"5.15")

LOCAL_PATH	:= $(call my-dir)
LOCAL_OUTPUT	:= $(AMBA_LINUX_OUT)
LOCAL_EXPORT	:= \
		ARCH=$(CPU_ARCH) \
		INSTALL_MOD_PATH=$(AMBA_LINUX_INSTALL) \
		-C $(AMBA_LINUX_SRC) \
		O=$(AMBA_LINUX_OUT) \
		LOCALVERSION=


ifeq ($(CONFIG_AMBARELLA_INITRAMFS), y)
LOCAL_EXPORT += CONFIG_INITRAMFS_SOURCE=${AMBA_IMAGE_OUT}/initramfs.cpio.gz
endif

###
linux: PRIVATE_EXPORT:=$(LOCAL_EXPORT)
linux: PRIVATE_OUTPUT:=$(LOCAL_OUTPUT)
linux:
	@mkdir -p $(PRIVATE_OUTPUT)
	@if [ -f $(PRIVATE_OUTPUT)/.config ]; then \
		echo "Build Linux-$(AMBA_LINUX_VERSION) with previous configuration ..."; \
	else \
		$(MAKE) $(AMBA_MAKE_PARA) defconfig_public_linux; \
	fi
	@$(MAKE) $(AMBA_MAKE_PARA) $(PRIVATE_EXPORT) olddefconfig
	@$(MAKE) $(AMBA_MAKE_PARA) $(PRIVATE_EXPORT) all
	@if [ -n "$(AMBA_LINUX_INSTALL)" ]; then \
		echo "Install Linux modules to $(AMBA_LINUX_INSTALL) ..."; \
		mkdir -p $(AMBA_LINUX_INSTALL); \
		$(MAKE) $(AMBA_MAKE_PARA) $(PRIVATE_EXPORT) modules_install; \
		find $(AMBA_LINUX_INSTALL)/lib/modules/ -name build | xargs -l1 rm -rf; \
		find $(AMBA_LINUX_INSTALL)/lib/modules/ -name source | xargs -l1 rm -rf; \
	fi

	@mkdir -p $(AMBA_IMAGE_OUT) \
		&& cp -dpRf $(PRIVATE_OUTPUT)/arch/$(CPU_ARCH)/boot/Image $(AMBA_IMAGE_OUT)/Image
	@echo "Build $@ Done."

$(call add-target-into-build, linux)


###
defconfig_public_linux: PRIVATE_EXPORT:=$(LOCAL_EXPORT)
defconfig_public_linux: PRIVATE_OUTPUT:=$(LOCAL_OUTPUT)
defconfig_public_linux: $(DOT_CONFIG) board
	@echo "Build Linux-$(AMBA_LINUX_VERSION) with $(AMBA_LINUX_DEFCONFIG) ..."
	@mkdir -p $(PRIVATE_OUTPUT)
	@if [ -f $(AMBA_SYSROOT_NATIVE)/share/config/linux/$(AMBA_LINUX_DEFCONFIG) ]; then \
		mkdir -p $(AMBA_LINUX_OUT); \
		cp -dpRf $(AMBA_SYSROOT_NATIVE)/share/config/linux/$(AMBA_LINUX_DEFCONFIG) $(AMBA_LINUX_OUT)/.config; \
	else \
		$(MAKE) $(AMBA_MAKE_PARA) $(PRIVATE_EXPORT) $(AMBA_LINUX_DEFCONFIG); \
	fi


###
menuconfig_public_linux: PRIVATE_EXPORT:=$(LOCAL_EXPORT)
menuconfig_public_linux: PRIVATE_OUTPUT:=$(LOCAL_OUTPUT)
menuconfig_public_linux: $(DOT_CONFIG)
	@if [ -f $(PRIVATE_OUTPUT)/.config ]; then \
		$(MAKE) $(AMBA_MAKE_PARA) $(PRIVATE_EXPORT) menuconfig; \
	else \
		$(MAKE) $(AMBA_MAKE_PARA) defconfig_public_linux; \
		$(MAKE) $(AMBA_MAKE_PARA) $(PRIVATE_EXPORT) menuconfig; \
	fi

endif

