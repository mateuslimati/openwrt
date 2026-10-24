include $(TOPDIR)/rules.mk

PKG_NAME:=ls1021aiot-kw41z
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define Package/ls1021aiot-kw41z
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=FRDM-KW41Z Bluetooth controller on the LS1021A-IoT
  DEPENDS:=@TARGET_layerscape_armv7 +kmod-bluetooth +bluez-utils +kmod-hci-uart
  PKGARCH:=all
endef

define Package/ls1021aiot-kw41z/description
  Attaches a FRDM-KW41Z running the LS1021aIOT_hci_black_box firmware on
  the Arduino header as hci0 (H4 over UART2, /dev/ttyS1, 115200 8N1, no
  flow control) and brings it up at boot. The J30/J31 jumpers of the FRDM
  must be crossed (J30-1 to J31-2, J30-2 to J31-1) to route its UART to the
  header. With CPLD image 2.10 the KW41Z is reset through the CPLD before
  it is attached.
endef

define Package/ls1021aiot-kw41z/conffiles
/etc/config/ls1021aiot_kw41z
endef

define Build/Compile
endef

define Package/ls1021aiot-kw41z/install
	$(INSTALL_DIR) $(1)/etc/config $(1)/etc/init.d
	$(INSTALL_CONF) ./files/ls1021aiot_kw41z.config $(1)/etc/config/ls1021aiot_kw41z
	$(INSTALL_BIN) ./files/ls1021aiot_kw41z.init $(1)/etc/init.d/ls1021aiot_kw41z
endef

$(eval $(call BuildPackage,ls1021aiot-kw41z))
