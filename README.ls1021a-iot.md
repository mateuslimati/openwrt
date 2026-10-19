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
| `kernel: add ls1021aiot-cpld LED driver` | LED0 da CPLD como LED do Linux (pisca 1x por segundo) e chaves SW2 em sysfs |
| `ls1021aiot-fan: set the fan threshold of the LS1021A-IoT at boot` | ventoinha liga a 65 °C (ADT7461 → CPLD) |
| `ls1021aiot-cpld: add the Arduino shield reset` | `shield_reset` em sysfs (CPLD 2.10) |
| `ls1021aiot-kw41z: attach the FRDM-KW41Z as hci0 at boot` | Bluetooth LE pela FRDM-KW41Z no conector Arduino |
| `spartan-gatt: add a Bluetooth LE GATT server for the LS1021A-IoT` | temperaturas, potência e LED0 pelo celular |
| `layerscape: ls1021a-iot: audio and HDMI` | SGTL5000 (SAI2) e SiI9022A (DCU) no kernel e no DTS |
| `layerscape: rtl8366_smi: SPI management for the RTL8365MB` | switch no kernel (swconfig `switch0`) pelo mesmo SPI do `rtlsw` |

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

- LED0 da CPLD (precisa da imagem da CPLD 2.9): `/sys/class/leds/ls1021aiot:green:led0`, com
  trigger `timer` (500 ms aceso, 500 ms apagado) por padrão. As chaves SW2 ficam em
  `/sys/bus/platform/devices/7fb00000.cpld/dip_switch` (chave em ON lida como 0).

- LuCI: `https://<ip da eth2>/`. Defina a senha de root no primeiro acesso.
- Ventoinha: liga quando o LS1021A passa de 65 °C e desliga abaixo de 55 °C. Ajuste em
  `/etc/config/ls1021aiot_fan` e rode `/etc/init.d/ls1021aiot_fan restart`.
- Bluetooth LE (FRDM-KW41Z com `LS1021aIOT_hci_black_box.bin`): sobe como `hci0` na `ttyS1`
  (115200, sem controle de fluxo). Na FRDM:
  - J30/J31 em cruz (J30-1↔J31-2, J30-2↔J31-1): a UART vai para o conector Arduino;
  - J28/J29 removidos e os pinos J2-3 a J2-6 (D10–D13) isolados: eles são o LED RGB e o SPI do
    KW41Z e caem no SPI1 do switch;
  - com a CPLD 2.10, o boot reseta o KW41Z pela CPLD antes do `hciattach`. Com versões
    anteriores, aperte o reset da FRDM depois do boot.

  ```sh
  hcitool -i hci0 lescan
  ```

- GATT (`spartan-gatt`): a placa anuncia como `spartan` e serve, sem pareamento:
  - Environmental Sensing (0x181A): temperatura do die do LS1021A e da placa (0x2A6E,
    leitura e notificação);
  - serviço `7370a000-4c53-3130-3231-412d496f5400`: LED0 (`a001`, UINT8: 0 apaga, 1 acende,
    2 pisca) e potência de entrada de 12 V em mW (`a002`, leitura e notificação).

  O firmware HCI do KW41Z trava se receber um pacote ACL fora da janela curta que segue cada
  pacote que ele envia. O servidor manda um comando HCI antes de cada resposta atrasada e de
  cada notificação, testa o controlador a cada 5 s e, se ele não responder, sai; o init o
  reinicia, resetando o KW41Z pela CPLD. Ele substitui o `bluetoothd` no `hci0`; desligue com
  `uci set spartan_gatt.gatt.enabled=0`.
- Switch:

```sh
rtlsw id          # chip 0x6000 ver 0x1000
rtlsw ports       # link das portas
rtlsw mib 1       # contadores da porta P1
rtlsw isol        # matriz de isolamento
```
