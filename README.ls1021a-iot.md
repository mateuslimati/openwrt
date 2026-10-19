# OpenWrt na LS1021A-IoT (silício rev 1.0)

Branch `ls1021aiot`, sobre o OpenWrt da NXP (`nxp-qoriq/openwrt`, commit `bd992def41`,
Linux 5.4.52, target `layerscape/armv7`, glibc). Leva a placa ao Linux 5.4 com LuCI, rede
estável e gerência do switch RTL8365MB.

O que a branch acrescenta:

| Commit | Para que serve |
|---|---|
| `layerscape: armv7: enable the DSPI SPI controller driver` | driver do DSPI no kernel |
| `layerscape: ls1021a-iot: expose the switch over spidev, fix CMA and eTSEC` | DTS: spidev no CS1 do DSPI1, CMA em lowmem, eTSEC sem `dma-coherent` |
| `layerscape: gianfar: no cache snooping when the node is not dma-coherent` | rede estável sob carga no rev 1.0 |
| `rtlsw: add RTL8365MB switch tool for the LS1021A-IoT` | `rtlsw` + init que liga a porta da CPU no switch |
| `layerscape: ls1021a-iot: use the switch port as LAN` | LAN = eth2 (switch) como cliente DHCP, sem servidor DHCP |

## Compilar

O OpenWrt de 2020 precisa de um ambiente antigo; use Ubuntu 20.04 (por exemplo em container):

```sh
git clone -b ls1021aiot https://github.com/mateuslimati/openwrt.git
cd openwrt
./scripts/feeds update -a && ./scripts/feeds install -a
cp ls1021a-iot.diffconfig .config
make defconfig
make download -j8
make -j8
```

O passo do kernel às vezes falha com `-j8` e passa na segunda vez. Se acontecer, rode
`make target/linux/compile -j1` e depois `make -j8` de novo.

Resultado:

- `bin/targets/layerscape/armv7-glibc/openwrt-layerscape-armv7-fsl_ls1021a-iot-sdboot-initramfs-kernel.bin`
- `build_dir/target-*/linux-layerscape_armv7/image-ls1021a-iot.dtb`

**Não grave o `…-sdcard.img` inteiro**: ele traz RCW e U-Boot próprios, que não bootam no
silício rev 1.0. Copie só o kernel e o DTB para a partição FAT do cartão e use o U-Boot da
branch `ls1021aiot` de `github.com/mateuslimati/u-boot`.

## Bootar

Com o U-Boot da branch `ls1021aiot` de `github.com/mateuslimati/u-boot`, copie o kernel e o
DTB para a partição FAT (por exemplo como `ow2.bin` e `ow2.dtb`) e grave no U-Boot:

```
setenv fdt_high 0xa0000000
setenv ow_kernel ow2.bin
setenv ow_dtb ow2.dtb
setenv ow_args setenv bootargs console=ttyS0,115200 panic=10
setenv owboot run ow_args\; mmc dev 0\; fatload mmc 0:1 0x82000000 \${ow_kernel}\; fatload mmc 0:1 0x8f000000 \${ow_dtb}\; bootm 0x82000000 - 0x8f000000
setenv bootcmd run owboot
saveenv
```

Esse U-Boot usa o parser simples: os `\;` e `\$` são necessários.

## Usar

- LuCI: `https://<ip da eth2>/`. Defina a senha de root no primeiro acesso.
- Switch:

```sh
rtlsw id          # chip 0x6000 ver 0x1000
rtlsw ports       # link das portas
rtlsw mib 1       # contadores da porta P1
rtlsw isol        # matriz de isolamento
```
