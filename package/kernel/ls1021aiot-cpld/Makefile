include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=ls1021aiot-cpld
PKG_RELEASE:=2
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define KernelPackage/ls1021aiot-cpld
  SUBMENU:=LED modules
  TITLE:=LS1021A-IoT system CPLD (LED0, GPIO, resets, watchdog)
  DEPENDS:=@TARGET_layerscape_armv7
  FILES:=$(PKG_BUILD_DIR)/ls1021aiot-cpld.ko
  AUTOLOAD:=$(call AutoLoad,30,ls1021aiot-cpld,1)
  KCONFIG:=
endef

define KernelPackage/ls1021aiot-cpld/description
 Driver for the system CPLD of the LS1021A-IoT board. Exposes LED0 as a
 Linux LED (with triggers) and the SW2 DIP switches in sysfs. LED0 needs
 CPLD image 2.9 or later. With CPLD image 3.0 it also gives a GPIO chip
 (J501, Arduino D2-D7, PCA9555 IO1, test points), a reset controller for
 the switch, PHYs, USB hub, shield, HDMI and mini PCIe slots, a watchdog,
 cold/warm/full reset and the board status in sysfs.
endef

MAKE_OPTS:= \
	$(KERNEL_MAKE_FLAGS) \
	M="$(PKG_BUILD_DIR)"

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C "$(LINUX_DIR)" $(MAKE_OPTS) modules
endef

$(eval $(call KernelPackage,ls1021aiot-cpld))
