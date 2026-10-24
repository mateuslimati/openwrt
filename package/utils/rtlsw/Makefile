include $(TOPDIR)/rules.mk

PKG_NAME:=rtlsw
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define Package/rtlsw
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=RTL8365MB switch control for LS1021A-IoT (SPI on DSPI1)
  DEPENDS:=@TARGET_layerscape_armv7
endef

define Package/rtlsw/description
  Reads and writes the Realtek RTL8365MB switch on the LS1021A-IoT board
  through spidev (DSPI1, CS1): link status, MIB counters, port isolation,
  internal PHYs and VLAN table. The init script brings up the CPU port
  (EXT1, RGMII 1000/FD) before the network starts.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Package/rtlsw/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/rtlsw $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/rtlsw.init $(1)/etc/init.d/rtlsw
endef

$(eval $(call BuildPackage,rtlsw))
