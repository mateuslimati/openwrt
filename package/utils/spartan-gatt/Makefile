include $(TOPDIR)/rules.mk

PKG_NAME:=spartan-gatt
PKG_VERSION:=1.0
PKG_RELEASE:=2
PKG_LICENSE:=GPL-2.0-or-later
PKG_BUILD_DEPENDS:=bluez

include $(INCLUDE_DIR)/package.mk

# links the static libshared-mainloop and libbluetooth-internal of the bluez
# build, which bluez does not install
BLUEZ_DIR:=$(BUILD_DIR)/bluez-5.54

define Package/spartan-gatt
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Bluetooth LE GATT server for the LS1021A-IoT
  DEPENDS:=@TARGET_layerscape_armv7 +kmod-bluetooth +ls1021aiot-kw41z
endef

define Package/spartan-gatt/description
  Serves the LS1021A die and board temperatures (Environmental Sensing),
  the 12 V input power and the CPLD LED0 over Bluetooth LE, through the
  FRDM-KW41Z on the Arduino header. It replaces bluetoothd on hci0.
  It plays an alert sound when a central connects and when it leaves.
endef

define Package/spartan-gatt/conffiles
/etc/config/spartan_gatt
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -DHAVE_CONFIG_H \
		-I$(BLUEZ_DIR) -I$(BLUEZ_DIR)/lib \
		-o $(PKG_BUILD_DIR)/spartan-gatt \
		./src/spartan-gatt.c $(BLUEZ_DIR)/src/uuid-helper.c \
		$(BLUEZ_DIR)/src/.libs/libshared-mainloop.a \
		$(BLUEZ_DIR)/lib/.libs/libbluetooth-internal.a \
		$(TARGET_LDFLAGS)
endef

define Package/spartan-gatt/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/libexec $(1)/etc/config $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/spartan-gatt $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/spartan-gatt-run $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/spartan-gatt-event $(1)/usr/libexec/
	$(INSTALL_CONF) ./files/spartan_gatt.config $(1)/etc/config/spartan_gatt
	$(INSTALL_BIN) ./files/spartan_gatt.init $(1)/etc/init.d/spartan_gatt
endef

$(eval $(call BuildPackage,spartan-gatt))
