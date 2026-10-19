include $(TOPDIR)/rules.mk

PKG_NAME:=ls1021aiot-fan
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define Package/ls1021aiot-fan
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Fan threshold for the LS1021A-IoT
  DEPENDS:=@TARGET_layerscape_armv7 +kmod-hwmon-lm90
  PKGARCH:=all
endef

define Package/ls1021aiot-fan/description
  On the LS1021A-IoT the fan is switched by the THERM output of the ADT7461
  that measures the LS1021A die, through the CPLD. Its factory limit is
  85 C, so the fan almost never runs. This sets the limit and hysteresis
  at boot from /etc/config/ls1021aiot_fan.
endef

define Package/ls1021aiot-fan/conffiles
/etc/config/ls1021aiot_fan
endef

define Build/Compile
endef

define Package/ls1021aiot-fan/install
	$(INSTALL_DIR) $(1)/etc/config $(1)/etc/init.d
	$(INSTALL_CONF) ./files/ls1021aiot_fan.config $(1)/etc/config/ls1021aiot_fan
	$(INSTALL_BIN) ./files/ls1021aiot_fan.init $(1)/etc/init.d/ls1021aiot_fan
endef

$(eval $(call BuildPackage,ls1021aiot-fan))
