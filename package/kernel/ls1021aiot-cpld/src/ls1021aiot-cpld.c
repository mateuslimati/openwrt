// SPDX-License-Identifier: GPL-2.0
/*
 * LS1021A-IoT system CPLD.
 *
 * The CPLD sits on IFC chip select 0 and only sees IFC_AD0-7, not the
 * address lines nor AVD.
 *
 * Images before 3.0 have a single 8-bit register at every address of the
 * window:
 *  - reads return the SW2 DIP switches (switch ON reads 0); only the first
 *    read after the CPLD powers up returns its version instead
 *  - writes go to a control register, default 0xF6: bit 0 drives LED0
 *    (CPLD image 2.9 or later), bits 1-3 select the SATA/SGMII and QE/LCD
 *    muxes and shut down the SerDes mux, bit 4 low holds the Arduino shield
 *    in reset (CPLD image 2.10 or later), bit 5 low makes the CPLD rebuild
 *    the HDMI HSYNC/VSYNC from DE (2.11 or later)
 * Reads never return what was written, so the driver keeps a copy of the
 * control register and never does read-modify-write on the hardware.
 *
 * Image 3.0 has addressed registers when CS0 runs in NOR ADM "AVD" mode,
 * where the IFC asserts CS before it drives the address on AD. After every
 * PORESET it behaves like 2.x until 0x3C is written at offset 0xC3. The
 * driver reads the version the CPLD put in GPPORCR1 at reset and only then
 * moves CS0 to NOR mode, so that an older CPLD never sees the unlock write.
 * On top of LED0, the DIP switches and the shield reset, 3.0 gives:
 *  - a GPIO chip with CPLD_EXPAND0-5 (J501), ARD_D2-D7, GPIO_EXPAND8-15
 *    (IO1 of the PCA9555) and TP9-16
 *  - a reset controller with the switch, both SGMII PHYs, the USB hub, the
 *    Arduino shield, the HDMI transmitter and both mini PCIe slots, also
 *    reachable from sysfs (reset/<chip>)
 *  - a watchdog in seconds
 *  - cold reboot, warm reset and full reset (as the SW1 button), from
 *    sysfs (action) or as the system restart (module parameter reboot)
 *  - the power-good, thermal, interrupt and sequencer status, and the cause
 *    of the last reset
 */

#include <linux/delay.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/reset-controller.h>
#include <linux/spinlock.h>
#include <linux/watchdog.h>

#define CPLD_CTRL_DEFAULT	0xf6
#define CPLD_CTRL_LED0		BIT(0)
#define CPLD_CTRL_SHIELD_RUN	BIT(4)
#define CPLD_CTRL_LCD_SYNC	BIT(5)

/* 3.0 registers */
#define CPLD_VER		0x00
#define CPLD_ID			0x01
#define CPLD_DIP		0x02
#define CPLD_POWER		0x03
#define CPLD_THERM		0x04
#define CPLD_IRQ		0x05
#define CPLD_EVT		0x06
#define CPLD_SEQ		0x07
#define CPLD_RST		0x08
#define CPLD_CTRL		0x09
#define CPLD_CTRL2		0x0a
#define CPLD_ACTION		0x0b
#define CPLD_WDT_CTRL		0x0c
#define CPLD_WDT_TIMEOUT	0x0d
#define CPLD_WDT_LEFT		0x0e
#define CPLD_CAUSE		0x0f
#define CPLD_SCRATCH		0x1c
#define CPLD_UNLOCK		0xc3

#define CPLD_ID_VALUE		0x1a
#define CPLD_UNLOCK_VALUE	0x3c
#define CPLD_ACTION_COLD	0xc0
#define CPLD_ACTION_WARM	0xa1
#define CPLD_ACTION_FULL	0xb0
#define CPLD_WDT_EN		BIT(0)
#define CPLD_WDT_COLD		BIT(1)
#define CPLD_CTRL2_FAN		BIT(0)
#define CPLD_CTRL2_I2C_OFF	BIT(1)
#define CPLD_CTRL2_QSPI_RST	BIT(2)
#define CPLD_CAUSE_WDT		BIT(3)

/* IFC registers of chip select 0, big-endian */
#define IFC_CSPR0		0x010
#define IFC_CSOR0		0x130
#define IFC_FTIM0(n)		(0x1c0 + 4 * (n))
#define IFC_CSPR_V		BIT(0)
#define IFC_CSPR_MSEL		GENMASK(2, 1)

/* GPPORCR1 in DCFG: the CPLD drives its version on IFC_AD0-7 at PORESET */
#define DCFG_GPPORCR1		0x20

/*
 * CS0 in NOR ADM AVD mode: 8 bits, ADM_SHFT_MODE = 1, NOR_MODE = 01 (CS
 * before AVD), with a long address phase so that the CPLD, which samples
 * AD with a 25 MHz clock, sees the address of every access.
 */
static const u32 ifc_nor_timing[3] = {
	0x10200410,	/* TACSE 1, TEADC 32, TAVDS 4, TEAHC 16 */
	0xff003f3f,	/* TACO 255, TRAD 63, TSEQRAD 63 */
	0x0f3c40ff,	/* TCS 15, TCH 15, TWPH 16, TWP 255 */
};
#define IFC_CSOR_NOR_AVD	0x80000020
#define IFC_CSPR_MSEL_NOR	0

static char *reboot = "";
module_param(reboot, charp, 0444);
MODULE_PARM_DESC(reboot, "restart the system with the CPLD: cold, warm or full (default: no)");

static bool wdt_cold;
module_param(wdt_cold, bool, 0444);
MODULE_PARM_DESC(wdt_cold, "the watchdog does a cold reboot instead of a warm reset");

struct ls1021aiot_cpld {
	struct device *dev;
	void __iomem *base;
	void __iomem *ifc;
	spinlock_t lock;
	u8 ctrl;		/* copy of the 2.x control register */
	u8 version;
	bool v3;
	u32 ifc_saved[5];	/* CSPR0, CSOR0, FTIM0-2 as U-Boot set them */
	struct led_classdev led;
	struct gpio_chip gc;
	struct reset_controller_dev rcdev;
	struct watchdog_device wdd;
};

static u8 cpld_read(struct ls1021aiot_cpld *cpld, unsigned int reg)
{
	return readb(cpld->base + reg);
}

static void cpld_write(struct ls1021aiot_cpld *cpld, unsigned int reg, u8 val)
{
	writeb(val, cpld->base + reg);
}

static void cpld_rmw(struct ls1021aiot_cpld *cpld, unsigned int reg, u8 mask,
		     u8 val)
{
	unsigned long flags;

	spin_lock_irqsave(&cpld->lock, flags);
	cpld_write(cpld, reg, (cpld_read(cpld, reg) & ~mask) | (val & mask));
	spin_unlock_irqrestore(&cpld->lock, flags);
}

/* the 2.x control register, or 0x09 of 3.0 */
static void ls1021aiot_cpld_update(struct ls1021aiot_cpld *cpld, u8 mask,
				   u8 val)
{
	unsigned long flags;

	if (cpld->v3) {
		cpld_rmw(cpld, CPLD_CTRL, mask, val);
		return;
	}

	spin_lock_irqsave(&cpld->lock, flags);
	cpld->ctrl = (cpld->ctrl & ~mask) | val;
	writeb(cpld->ctrl, cpld->base);
	spin_unlock_irqrestore(&cpld->lock, flags);
}

static u8 ls1021aiot_cpld_ctrl(struct ls1021aiot_cpld *cpld)
{
	return cpld->v3 ? cpld_read(cpld, CPLD_CTRL) : cpld->ctrl;
}

static void ls1021aiot_cpld_led_set(struct led_classdev *led,
				    enum led_brightness brightness)
{
	struct ls1021aiot_cpld *cpld =
		container_of(led, struct ls1021aiot_cpld, led);

	ls1021aiot_cpld_update(cpld, CPLD_CTRL_LED0,
			       brightness ? CPLD_CTRL_LED0 : 0);
}

/* IFC chip select 0 */

static void ifc_cs0_set(struct ls1021aiot_cpld *cpld, u32 cspr, u32 csor,
			const u32 *ftim)
{
	int i;

	/* invalidate CS0 while its timing changes */
	iowrite32be(cspr & ~IFC_CSPR_V, cpld->ifc + IFC_CSPR0);
	iowrite32be(csor, cpld->ifc + IFC_CSOR0);
	for (i = 0; i < 3; i++)
		iowrite32be(ftim[i], cpld->ifc + IFC_FTIM0(i));
	iowrite32be(cspr, cpld->ifc + IFC_CSPR0);
	ioread32be(cpld->ifc + IFC_CSPR0);
}

static void ifc_cs0_restore(struct ls1021aiot_cpld *cpld)
{
	ifc_cs0_set(cpld, cpld->ifc_saved[0], cpld->ifc_saved[1],
		    &cpld->ifc_saved[2]);
}

/*
 * Read the version from GPPORCR1 and, for 3.0 or later, move CS0 to NOR
 * mode and unlock the registers. Returns true when they answer.
 */
static bool ls1021aiot_cpld_v3_init(struct ls1021aiot_cpld *cpld)
{
	struct device_node *np;
	void __iomem *dcfg;
	u32 cspr;
	int i;

	np = of_find_compatible_node(NULL, NULL, "fsl,ls1021a-dcfg");
	if (!np)
		return false;
	dcfg = of_iomap(np, 0);
	of_node_put(np);
	if (!dcfg)
		return false;
	cpld->version = ioread32be(dcfg + DCFG_GPPORCR1) >> 24;
	iounmap(dcfg);

	if (cpld->version < 0x30)
		return false;

	np = of_get_parent(cpld->dev->of_node);
	cpld->ifc = of_iomap(np, 0);
	of_node_put(np);
	if (!cpld->ifc)
		return false;

	cpld->ifc_saved[0] = ioread32be(cpld->ifc + IFC_CSPR0);
	cpld->ifc_saved[1] = ioread32be(cpld->ifc + IFC_CSOR0);
	for (i = 0; i < 3; i++)
		cpld->ifc_saved[2 + i] = ioread32be(cpld->ifc + IFC_FTIM0(i));

	cspr = (cpld->ifc_saved[0] & ~IFC_CSPR_MSEL) | IFC_CSPR_MSEL_NOR;
	ifc_cs0_set(cpld, cspr, IFC_CSOR_NOR_AVD, ifc_nor_timing);
	cpld_write(cpld, CPLD_UNLOCK, CPLD_UNLOCK_VALUE);

	if (cpld_read(cpld, CPLD_ID) == CPLD_ID_VALUE &&
	    cpld_read(cpld, CPLD_VER) == cpld->version)
		return true;

	dev_warn(cpld->dev, "CPLD %d.%d does not answer in NOR mode, using the 2.x interface\n",
		 cpld->version >> 4, cpld->version & 0xf);
	ifc_cs0_restore(cpld);
	iounmap(cpld->ifc);
	cpld->ifc = NULL;
	return false;
}

/* GPIO: four banks, each with direction, output and input registers */

struct cpld_bank {
	u8 reg;		/* direction; output is reg + 1, input reg + 2 */
	u8 first;	/* first GPIO */
	u8 count;
	u8 in_only;	/* bits that cannot be outputs */
};

static const struct cpld_bank cpld_banks[] = {
	{ 0x10, 0, 6, 0 },		/* CPLD_EXPAND0-5 */
	{ 0x13, 6, 6, 0 },		/* ARD_PD2-7 */
	{ 0x16, 12, 8, BIT(7) },	/* GPIO_EXPAND8-15 */
	{ 0x19, 20, 8, 0 },		/* TP9-16 */
};
#define CPLD_NGPIO	28

static const char * const cpld_gpio_names[CPLD_NGPIO] = {
	"CPLD_EXPAND0", "CPLD_EXPAND1", "CPLD_EXPAND2", "CPLD_EXPAND3",
	"CPLD_EXPAND4", "CPLD_EXPAND5",
	"ARD_D2", "ARD_D3", "ARD_D4", "ARD_D5", "ARD_D6", "ARD_D7",
	"GPIO_EXPAND8", "GPIO_EXPAND9", "GPIO_EXPAND10", "GPIO_EXPAND11",
	"GPIO_EXPAND12", "GPIO_EXPAND13", "GPIO_EXPAND14", "GPIO_EXPAND15",
	"TP9", "TP10", "TP11", "TP12", "TP13", "TP14", "TP15", "TP16",
};

static const struct cpld_bank *cpld_bank(unsigned int offset, u8 *bit)
{
	const struct cpld_bank *b = cpld_banks;

	while (offset >= b->first + b->count)
		b++;
	*bit = BIT(offset - b->first);
	return b;
}

static int cpld_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct ls1021aiot_cpld *cpld = gpiochip_get_data(gc);
	const struct cpld_bank *b;
	u8 bit;

	b = cpld_bank(offset, &bit);
	return cpld_read(cpld, b->reg) & bit ? GPIO_LINE_DIRECTION_OUT :
					       GPIO_LINE_DIRECTION_IN;
}

static int cpld_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct ls1021aiot_cpld *cpld = gpiochip_get_data(gc);
	const struct cpld_bank *b;
	u8 bit;

	b = cpld_bank(offset, &bit);
	cpld_rmw(cpld, b->reg, bit, 0);
	return 0;
}

static int cpld_gpio_direction_output(struct gpio_chip *gc,
				      unsigned int offset, int value)
{
	struct ls1021aiot_cpld *cpld = gpiochip_get_data(gc);
	const struct cpld_bank *b;
	u8 bit;

	b = cpld_bank(offset, &bit);
	if (b->in_only & bit)
		return -EINVAL;
	/* set the level first, so that the line starts at it */
	cpld_rmw(cpld, b->reg + 1, bit, value ? bit : 0);
	cpld_rmw(cpld, b->reg, bit, bit);
	return 0;
}

static int cpld_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct ls1021aiot_cpld *cpld = gpiochip_get_data(gc);
	const struct cpld_bank *b;
	u8 bit;

	b = cpld_bank(offset, &bit);
	return !!(cpld_read(cpld, b->reg + 2) & bit);
}

static int cpld_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct ls1021aiot_cpld *cpld = gpiochip_get_data(gc);
	const struct cpld_bank *b;
	u8 bit;

	b = cpld_bank(offset, &bit);
	cpld_rmw(cpld, b->reg + 1, bit, value ? bit : 0);
	return 0;
}

/* resets: one bit per chip in 0x08, 1 = running */

static const char * const cpld_reset_names[8] = {
	"switch", "sgmii1_phy", "sgmii2_phy", "usb_hub",
	"arduino_shield", "hdmi", "pcie1", "pcie2",
};

static int cpld_reset_assert(struct reset_controller_dev *rcdev,
			     unsigned long id)
{
	struct ls1021aiot_cpld *cpld =
		container_of(rcdev, struct ls1021aiot_cpld, rcdev);

	cpld_rmw(cpld, CPLD_RST, BIT(id), 0);
	return 0;
}

static int cpld_reset_deassert(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct ls1021aiot_cpld *cpld =
		container_of(rcdev, struct ls1021aiot_cpld, rcdev);

	cpld_rmw(cpld, CPLD_RST, BIT(id), BIT(id));
	return 0;
}

static int cpld_reset_reset(struct reset_controller_dev *rcdev,
			    unsigned long id)
{
	cpld_reset_assert(rcdev, id);
	msleep(10);
	return cpld_reset_deassert(rcdev, id);
}

static int cpld_reset_status(struct reset_controller_dev *rcdev,
			     unsigned long id)
{
	struct ls1021aiot_cpld *cpld =
		container_of(rcdev, struct ls1021aiot_cpld, rcdev);

	return !(cpld_read(cpld, CPLD_RST) & BIT(id));
}

static const struct reset_control_ops cpld_reset_ops = {
	.assert = cpld_reset_assert,
	.deassert = cpld_reset_deassert,
	.reset = cpld_reset_reset,
	.status = cpld_reset_status,
};

/* watchdog */

static int cpld_wdt_start(struct watchdog_device *wdd)
{
	struct ls1021aiot_cpld *cpld = watchdog_get_drvdata(wdd);

	cpld_write(cpld, CPLD_WDT_TIMEOUT, wdd->timeout);
	cpld_write(cpld, CPLD_WDT_CTRL, CPLD_WDT_EN | (wdt_cold ? CPLD_WDT_COLD : 0));
	return 0;
}

static int cpld_wdt_stop(struct watchdog_device *wdd)
{
	struct ls1021aiot_cpld *cpld = watchdog_get_drvdata(wdd);

	cpld_write(cpld, CPLD_WDT_CTRL, 0);
	return 0;
}

static int cpld_wdt_ping(struct watchdog_device *wdd)
{
	struct ls1021aiot_cpld *cpld = watchdog_get_drvdata(wdd);

	cpld_write(cpld, CPLD_WDT_LEFT, 0);
	return 0;
}

static int cpld_wdt_set_timeout(struct watchdog_device *wdd, unsigned int t)
{
	struct ls1021aiot_cpld *cpld = watchdog_get_drvdata(wdd);

	wdd->timeout = t;
	/* writing the timeout also reloads the counter */
	cpld_write(cpld, CPLD_WDT_TIMEOUT, t);
	return 0;
}

static unsigned int cpld_wdt_get_timeleft(struct watchdog_device *wdd)
{
	struct ls1021aiot_cpld *cpld = watchdog_get_drvdata(wdd);

	return cpld_read(cpld, CPLD_WDT_LEFT);
}

static const struct watchdog_info cpld_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE |
		   WDIOF_CARDRESET,
	.identity = "LS1021A-IoT CPLD watchdog",
};

static const struct watchdog_ops cpld_wdt_ops = {
	.owner = THIS_MODULE,
	.start = cpld_wdt_start,
	.stop = cpld_wdt_stop,
	.ping = cpld_wdt_ping,
	.set_timeout = cpld_wdt_set_timeout,
	.get_timeleft = cpld_wdt_get_timeleft,
};

/* restart */

static int cpld_action_code(const char *name)
{
	if (sysfs_streq(name, "cold"))
		return CPLD_ACTION_COLD;
	if (sysfs_streq(name, "warm"))
		return CPLD_ACTION_WARM;
	if (sysfs_streq(name, "full"))
		return CPLD_ACTION_FULL;
	return -EINVAL;
}

static int cpld_restart(struct sys_off_data *data)
{
	struct ls1021aiot_cpld *cpld = data->cb_data;

	cpld_write(cpld, CPLD_ACTION, cpld_action_code(reboot));
	mdelay(3000);
	return NOTIFY_DONE;
}

/* sysfs */

static ssize_t shield_reset_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct ls1021aiot_cpld *cpld = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n",
		       !(ls1021aiot_cpld_ctrl(cpld) & CPLD_CTRL_SHIELD_RUN));
}

/*
 * 1 holds the shield in reset, 0 releases it, "pulse" resets it for 10 ms.
 * Needs CPLD image 2.10; older images ignore bit 4.
 */
static ssize_t shield_reset_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct ls1021aiot_cpld *cpld = dev_get_drvdata(dev);
	bool assert;

	if (sysfs_streq(buf, "pulse")) {
		ls1021aiot_cpld_update(cpld, CPLD_CTRL_SHIELD_RUN, 0);
		msleep(10);
		ls1021aiot_cpld_update(cpld, CPLD_CTRL_SHIELD_RUN,
				       CPLD_CTRL_SHIELD_RUN);
		return count;
	}

	if (kstrtobool(buf, &assert))
		return -EINVAL;

	ls1021aiot_cpld_update(cpld, CPLD_CTRL_SHIELD_RUN,
			       assert ? 0 : CPLD_CTRL_SHIELD_RUN);
	return count;
}
static DEVICE_ATTR_RW(shield_reset);

static ssize_t dip_switch_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct ls1021aiot_cpld *cpld = dev_get_drvdata(dev);

	return sprintf(buf, "0x%02x\n",
		       cpld_read(cpld, cpld->v3 ? CPLD_DIP : 0));
}
static DEVICE_ATTR_RO(dip_switch);

static struct attribute *cpld_attrs[] = {
	&dev_attr_shield_reset.attr,
	&dev_attr_dip_switch.attr,
	NULL
};

static const struct attribute_group cpld_group = {
	.attrs = cpld_attrs,
};

static ssize_t version_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct ls1021aiot_cpld *cpld = dev_get_drvdata(dev);

	return sprintf(buf, "%d.%d\n", cpld->version >> 4, cpld->version & 0xf);
}
static DEVICE_ATTR_RO(version);

struct cpld_bit {
	const char *name;
	u8 reg;
	u8 bit;
};

/* status lines, as the level of the pin (most are active low) */
static const struct cpld_bit cpld_status_bits[] = {
	{ "ps_vcc33_pg", CPLD_POWER, 0 },
	{ "ps_vcc33_pmic_pg", CPLD_POWER, 1 },
	{ "ps_vcc50_pg", CPLD_POWER, 2 },
	{ "pmic_por_b", CPLD_POWER, 3 },
	{ "poe_stat", CPLD_POWER, 4 },
	{ "usbhub_vbus", CPLD_POWER, 5 },
	{ "pwren", CPLD_POWER, 6 },
	{ "sda_swd_en", CPLD_POWER, 7 },
	{ "therm_warn_b", CPLD_THERM, 0 },
	{ "therm_fault_b", CPLD_THERM, 1 },
	{ "temp_monitor_os_b", CPLD_THERM, 2 },
	{ "fan_on", CPLD_THERM, 3 },
	{ "cec_d", CPLD_THERM, 4 },
	{ "sgmii1_phy_int_b", CPLD_IRQ, 0 },
	{ "sgmii2_phy_int_b", CPLD_IRQ, 1 },
	{ "eth_sw_int_b", CPLD_IRQ, 2 },
	{ "hdmi_irq", CPLD_IRQ, 3 },
	{ "pmic_int_b", CPLD_IRQ, 4 },
	{ "mma8451q_irq_b", CPLD_IRQ, 5 },
	{ "gpio_expand_int_b", CPLD_IRQ, 6 },
	{ "reset_req_b", CPLD_IRQ, 7 },
	{ "evt0_b", CPLD_EVT, 0 },
	{ "evt1_b", CPLD_EVT, 1 },
	{ "evt2_b", CPLD_EVT, 2 },
	{ "evt3_b", CPLD_EVT, 3 },
	{ "evt4_b", CPLD_EVT, 4 },
	{ "jtag_hrst_b", CPLD_EVT, 5 },
	{ "cpld_trst_b", CPLD_EVT, 6 },
	{ "hreset_b", CPLD_EVT, 7 },
	{ "power_state0", CPLD_SEQ, 0 },
	{ "power_state1", CPLD_SEQ, 1 },
	{ "power_state2", CPLD_SEQ, 2 },
	{ "power_state3", CPLD_SEQ, 3 },
	{ "reset_state0", CPLD_SEQ, 4 },
	{ "reset_state1", CPLD_SEQ, 5 },
	{ "reset_state2", CPLD_SEQ, 6 },
	{ "reset_state3", CPLD_SEQ, 7 },
};

static ssize_t status_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct ls1021aiot_cpld *cpld = dev_get_drvdata(dev);
	u8 regs[CPLD_SEQ + 1];
	ssize_t len = 0;
	int i;

	for (i = CPLD_POWER; i <= CPLD_SEQ; i++)
		regs[i] = cpld_read(cpld, i);
	for (i = 0; i < ARRAY_SIZE(cpld_status_bits); i++)
		len += sysfs_emit_at(buf, len, "%s %d\n", cpld_status_bits[i].name,
				     !!(regs[cpld_status_bits[i].reg] &
					BIT(cpld_status_bits[i].bit)));
	return len;
}
static DEVICE_ATTR_RO(status);

static ssize_t reset_cause_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ls1021aiot_cpld *cpld = dev_get_drvdata(dev);
	u8 c = cpld_read(cpld, CPLD_CAUSE);
	const char *s;

	if (c & 0x10)
		s = "reset_req";	/* RESET_REQ_B, the Linux reboot */
	else if (c & 0x08)
		s = c & 0x02 ? "watchdog_cold" : "watchdog_warm";
	else if (c & 0x02)
		s = "cold";
	else if (c & 0x04)
		s = "warm";
	else
		s = "power_on";		/* or the full reset, as SW1 */
	return sprintf(buf, "%s 0x%02x\n", s, c);
}
static DEVICE_ATTR_RO(reset_cause);

/* cold, warm or full */
static ssize_t action_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct ls1021aiot_cpld *cpld = dev_get_drvdata(dev);
	int code = cpld_action_code(buf);

	if (code < 0)
		return code;
	cpld_write(cpld, CPLD_ACTION, code);
	return count;
}
static DEVICE_ATTR_WO(action);

static ssize_t cpld_ctrl2_show(struct device *dev, char *buf, u8 bit)
{
	struct ls1021aiot_cpld *cpld = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", !!(cpld_read(cpld, CPLD_CTRL2) & bit));
}

static ssize_t cpld_ctrl2_store(struct device *dev, const char *buf,
				size_t count, u8 bit)
{
	struct ls1021aiot_cpld *cpld = dev_get_drvdata(dev);
	bool on;

	if (kstrtobool(buf, &on))
		return -EINVAL;
	cpld_rmw(cpld, CPLD_CTRL2, bit, on ? bit : 0);
	return count;
}

#define CPLD_CTRL2_ATTR(_name, _bit)						\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)		\
{										\
	return cpld_ctrl2_show(dev, buf, _bit);					\
}										\
static ssize_t _name##_store(struct device *dev,				\
			     struct device_attribute *attr,			\
			     const char *buf, size_t count)			\
{										\
	return cpld_ctrl2_store(dev, buf, count, _bit);				\
}										\
static DEVICE_ATTR_RW(_name)

/* 1 keeps the fan on, whatever the ADT7461 THERM pin says */
CPLD_CTRL2_ATTR(fan_force, CPLD_CTRL2_FAN);
/* 1 turns off the PCA9515 repeater between I2C1 and the Arduino header */
CPLD_CTRL2_ATTR(arduino_i2c_off, CPLD_CTRL2_I2C_OFF);
/* 1 holds the QSPI boot flash in reset */
CPLD_CTRL2_ATTR(qspi_reset, CPLD_CTRL2_QSPI_RST);

/* every register, for debugging */
static ssize_t regs_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct ls1021aiot_cpld *cpld = dev_get_drvdata(dev);
	ssize_t len = 0;
	int i;

	for (i = 0; i <= CPLD_SCRATCH; i++)
		len += sysfs_emit_at(buf, len, "%02x%c", cpld_read(cpld, i),
				     (i & 7) == 7 || i == CPLD_SCRATCH ? '\n' : ' ');
	return len;
}
static DEVICE_ATTR_RO(regs);

static struct attribute *cpld_v3_attrs[] = {
	&dev_attr_version.attr,
	&dev_attr_status.attr,
	&dev_attr_reset_cause.attr,
	&dev_attr_action.attr,
	&dev_attr_fan_force.attr,
	&dev_attr_arduino_i2c_off.attr,
	&dev_attr_qspi_reset.attr,
	&dev_attr_regs.attr,
	NULL
};

static const struct attribute_group cpld_v3_group = {
	.attrs = cpld_v3_attrs,
};

/* reset/<chip>: 1 holds it in reset, 0 runs it, "pulse" resets it for 10 ms */
struct cpld_reset_attr {
	struct device_attribute attr;
	unsigned long id;
};

static ssize_t chip_reset_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct ls1021aiot_cpld *cpld = dev_get_drvdata(dev);
	struct cpld_reset_attr *ra = container_of(attr, struct cpld_reset_attr, attr);

	return sprintf(buf, "%d\n", cpld_reset_status(&cpld->rcdev, ra->id));
}

static ssize_t chip_reset_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct ls1021aiot_cpld *cpld = dev_get_drvdata(dev);
	struct cpld_reset_attr *ra = container_of(attr, struct cpld_reset_attr, attr);
	bool assert;

	if (sysfs_streq(buf, "pulse")) {
		cpld_reset_reset(&cpld->rcdev, ra->id);
		return count;
	}
	if (kstrtobool(buf, &assert))
		return -EINVAL;
	if (assert)
		cpld_reset_assert(&cpld->rcdev, ra->id);
	else
		cpld_reset_deassert(&cpld->rcdev, ra->id);
	return count;
}

static struct cpld_reset_attr cpld_reset_attrs[8];
static struct attribute *cpld_reset_attr_list[9];

static const struct attribute_group cpld_reset_group = {
	.name = "reset",
	.attrs = cpld_reset_attr_list,
};

static int ls1021aiot_cpld_v3_probe(struct ls1021aiot_cpld *cpld)
{
	struct device *dev = cpld->dev;
	int i, ret;

	cpld->gc.label = "ls1021aiot-cpld";
	cpld->gc.parent = dev;
	cpld->gc.owner = THIS_MODULE;
	cpld->gc.base = -1;
	cpld->gc.ngpio = CPLD_NGPIO;
	cpld->gc.names = cpld_gpio_names;
	cpld->gc.can_sleep = false;
	cpld->gc.get_direction = cpld_gpio_get_direction;
	cpld->gc.direction_input = cpld_gpio_direction_input;
	cpld->gc.direction_output = cpld_gpio_direction_output;
	cpld->gc.get = cpld_gpio_get;
	cpld->gc.set = cpld_gpio_set;
	ret = devm_gpiochip_add_data(dev, &cpld->gc, cpld);
	if (ret)
		return ret;

	cpld->rcdev.owner = THIS_MODULE;
	cpld->rcdev.ops = &cpld_reset_ops;
	cpld->rcdev.of_node = dev->of_node;
	cpld->rcdev.nr_resets = ARRAY_SIZE(cpld_reset_names);
	ret = devm_reset_controller_register(dev, &cpld->rcdev);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(cpld_reset_names); i++) {
		struct cpld_reset_attr *ra = &cpld_reset_attrs[i];

		sysfs_attr_init(&ra->attr.attr);
		ra->attr.attr.name = cpld_reset_names[i];
		ra->attr.attr.mode = 0644;
		ra->attr.show = chip_reset_show;
		ra->attr.store = chip_reset_store;
		ra->id = i;
		cpld_reset_attr_list[i] = &ra->attr.attr;
	}
	ret = devm_device_add_group(dev, &cpld_v3_group);
	if (ret)
		return ret;
	ret = devm_device_add_group(dev, &cpld_reset_group);
	if (ret)
		return ret;

	cpld->wdd.info = &cpld_wdt_info;
	cpld->wdd.ops = &cpld_wdt_ops;
	cpld->wdd.parent = dev;
	cpld->wdd.min_timeout = 1;
	cpld->wdd.max_timeout = 255;
	cpld->wdd.timeout = 30;
	if (cpld_read(cpld, CPLD_CAUSE) & CPLD_CAUSE_WDT)
		cpld->wdd.bootstatus = WDIOF_CARDRESET;
	watchdog_init_timeout(&cpld->wdd, 0, dev);
	watchdog_set_drvdata(&cpld->wdd, cpld);
	watchdog_stop_on_unregister(&cpld->wdd);
	/* a watchdog left running (by U-Boot) is handed to userspace */
	if (cpld_read(cpld, CPLD_WDT_CTRL) & CPLD_WDT_EN)
		set_bit(WDOG_HW_RUNNING, &cpld->wdd.status);
	ret = devm_watchdog_register_device(dev, &cpld->wdd);
	if (ret)
		return ret;

	if (cpld_action_code(reboot) > 0) {
		ret = devm_register_restart_handler(dev, cpld_restart, cpld);
		if (ret)
			return ret;
	}

	return 0;
}

/* back to what U-Boot set up, with the CPLD in its 2.x mode */
static void ls1021aiot_cpld_v3_exit(void *data)
{
	struct ls1021aiot_cpld *cpld = data;

	cpld->ctrl = cpld_read(cpld, CPLD_CTRL);
	cpld_write(cpld, CPLD_UNLOCK, 0);
	cpld->v3 = false;
	ifc_cs0_restore(cpld);
	iounmap(cpld->ifc);
}

static int ls1021aiot_cpld_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct ls1021aiot_cpld *cpld;
	int ret;

	cpld = devm_kzalloc(&pdev->dev, sizeof(*cpld), GFP_KERNEL);
	if (!cpld)
		return -ENOMEM;

	cpld->dev = &pdev->dev;
	cpld->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cpld->base))
		return PTR_ERR(cpld->base);

	spin_lock_init(&cpld->lock);
	cpld->ctrl = CPLD_CTRL_DEFAULT;
	platform_set_drvdata(pdev, cpld);

	cpld->v3 = ls1021aiot_cpld_v3_init(cpld);
	if (cpld->v3) {
		ret = devm_add_action_or_reset(&pdev->dev, ls1021aiot_cpld_v3_exit, cpld);
		if (ret)
			return ret;
	}

	/*
	 * On rev 1.0 silicon the DCU sync pins are not available with the
	 * console RCW: let the CPLD regenerate them from DE.
	 */
	if (of_property_read_bool(np, "fsl,lcd-sync-from-de"))
		ls1021aiot_cpld_update(cpld, CPLD_CTRL_LCD_SYNC, 0);

	cpld->led.name = of_get_property(np, "label", NULL) ?: "ls1021aiot:green:led0";
	cpld->led.default_trigger = of_get_property(np, "linux,default-trigger", NULL);
	cpld->led.max_brightness = 1;
	cpld->led.brightness_set = ls1021aiot_cpld_led_set;

	ret = devm_led_classdev_register(&pdev->dev, &cpld->led);
	if (ret)
		return ret;

	ret = devm_device_add_group(&pdev->dev, &cpld_group);
	if (ret)
		return ret;

	if (cpld->v3) {
		ret = ls1021aiot_cpld_v3_probe(cpld);
		if (ret)
			return ret;
		dev_info(&pdev->dev, "CPLD %d.%d: registers, GPIO, resets and watchdog (SW2 = 0x%02x, last reset 0x%02x)\n",
			 cpld->version >> 4, cpld->version & 0xf,
			 cpld_read(cpld, CPLD_DIP), cpld_read(cpld, CPLD_CAUSE));
	} else {
		dev_info(&pdev->dev, "LED0 and DIP switches (SW2 = 0x%02x)\n",
			 readb(cpld->base));
	}
	return 0;
}

static const struct of_device_id ls1021aiot_cpld_of_match[] = {
	{ .compatible = "fsl,ls1021aiot-cpld" },
	{ }
};
MODULE_DEVICE_TABLE(of, ls1021aiot_cpld_of_match);

static struct platform_driver ls1021aiot_cpld_driver = {
	.probe = ls1021aiot_cpld_probe,
	.driver = {
		.name = "ls1021aiot-cpld",
		.of_match_table = ls1021aiot_cpld_of_match,
	},
};
module_platform_driver(ls1021aiot_cpld_driver);

MODULE_AUTHOR("Mateus Lima Alves <mateuslima.ti@gmail.com>");
MODULE_DESCRIPTION("LS1021A-IoT system CPLD");
MODULE_LICENSE("GPL v2");
