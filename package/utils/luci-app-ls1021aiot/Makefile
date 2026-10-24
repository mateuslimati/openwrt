include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-ls1021aiot
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-ls1021aiot
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=LuCI page of the LS1021A-IoT board
  DEPENDS:=@TARGET_layerscape_armv7 +luci-base +rpcd +swconfig
  PKGARCH:=all
endef

define Package/luci-app-ls1021aiot/description
  Status > LS1021A-IoT: the board temperatures, the voltage, current and
  power of the 12 V input and of the 1.0 V rail with their last 24 hours
  as graphs, the fan state, and the switch ports, which it can power
  down and up. ls1021aiot-sensord samples the sensors every 10 s into
  /tmp.
endef

define Build/Compile
endef

define Package/luci-app-ls1021aiot/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/usr/libexec/rpcd \
		$(1)/usr/share/rpcd/acl.d $(1)/usr/share/luci/menu.d \
		$(1)/www/luci-static/resources/view/ls1021aiot
	$(INSTALL_BIN) ./files/ls1021aiot-sensord $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/ls1021aiot-sensord.init $(1)/etc/init.d/ls1021aiot-sensord
	$(INSTALL_BIN) ./files/rpcd-ls1021aiot $(1)/usr/libexec/rpcd/ls1021aiot
	$(INSTALL_DATA) ./files/luci-app-ls1021aiot.acl.json \
		$(1)/usr/share/rpcd/acl.d/luci-app-ls1021aiot.json
	$(INSTALL_DATA) ./files/luci-app-ls1021aiot.menu.json \
		$(1)/usr/share/luci/menu.d/luci-app-ls1021aiot.json
	$(INSTALL_DATA) ./files/board.js \
		$(1)/www/luci-static/resources/view/ls1021aiot/
endef

define Package/luci-app-ls1021aiot/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/ls1021aiot-sensord enable
/etc/init.d/ls1021aiot-sensord start
/etc/init.d/rpcd reload
rm -rf /tmp/luci-indexcache* /tmp/luci-modulecache
exit 0
endef

$(eval $(call BuildPackage,luci-app-ls1021aiot))
