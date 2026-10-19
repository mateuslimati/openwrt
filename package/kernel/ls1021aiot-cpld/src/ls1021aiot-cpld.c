// SPDX-License-Identifier: GPL-2.0
/*
 * LS1021A-IoT system CPLD: LED0 and the SW2 DIP switches.
 *
 * The CPLD sits on IFC chip select 0 as a single 8-bit register (every
 * address of the window hits it):
 *  - reads return the SW2 DIP switches (switch ON reads 0); only the first
 *    read after the CPLD powers up returns its version instead
 *  - writes go to a control register, default 0xF6: bit 0 drives LED0
 *    (CPLD image 2.9 or later), bits 1-3 select the SATA/SGMII and QE/LCD
 *    muxes and shut down the SerDes mux
 *
 * Reads never return what was written, so the driver keeps a copy of the
 * control register and never does read-modify-write on the hardware.
 */

#include <linux/io.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define CPLD_CTRL_DEFAULT	0xf6
#define CPLD_CTRL_LED0		BIT(0)

struct ls1021aiot_cpld {
	void __iomem *base;
	spinlock_t lock;
	u8 ctrl;
	struct led_classdev led;
};

static void ls1021aiot_cpld_led_set(struct led_classdev *led,
				    enum led_brightness brightness)
{
	struct ls1021aiot_cpld *cpld =
		container_of(led, struct ls1021aiot_cpld, led);
	unsigned long flags;

	spin_lock_irqsave(&cpld->lock, flags);
	if (brightness)
		cpld->ctrl |= CPLD_CTRL_LED0;
	else
		cpld->ctrl &= ~CPLD_CTRL_LED0;
	writeb(cpld->ctrl, cpld->base);
	spin_unlock_irqrestore(&cpld->lock, flags);
}

static ssize_t dip_switch_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct ls1021aiot_cpld *cpld = dev_get_drvdata(dev);

	return sprintf(buf, "0x%02x\n", readb(cpld->base));
}
static DEVICE_ATTR_RO(dip_switch);

static int ls1021aiot_cpld_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct ls1021aiot_cpld *cpld;
	int ret;

	cpld = devm_kzalloc(&pdev->dev, sizeof(*cpld), GFP_KERNEL);
	if (!cpld)
		return -ENOMEM;

	cpld->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cpld->base))
		return PTR_ERR(cpld->base);

	spin_lock_init(&cpld->lock);
	cpld->ctrl = CPLD_CTRL_DEFAULT;
	platform_set_drvdata(pdev, cpld);

	cpld->led.name = of_get_property(np, "label", NULL) ?: "ls1021aiot:green:led0";
	cpld->led.default_trigger = of_get_property(np, "linux,default-trigger", NULL);
	cpld->led.max_brightness = 1;
	cpld->led.brightness_set = ls1021aiot_cpld_led_set;

	ret = devm_led_classdev_register(&pdev->dev, &cpld->led);
	if (ret)
		return ret;

	ret = device_create_file(&pdev->dev, &dev_attr_dip_switch);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "LED0 and DIP switches (SW2 = 0x%02x)\n",
		 readb(cpld->base));
	return 0;
}

static int ls1021aiot_cpld_remove(struct platform_device *pdev)
{
	device_remove_file(&pdev->dev, &dev_attr_dip_switch);
	return 0;
}

static const struct of_device_id ls1021aiot_cpld_of_match[] = {
	{ .compatible = "fsl,ls1021aiot-cpld" },
	{ }
};
MODULE_DEVICE_TABLE(of, ls1021aiot_cpld_of_match);

static struct platform_driver ls1021aiot_cpld_driver = {
	.probe = ls1021aiot_cpld_probe,
	.remove = ls1021aiot_cpld_remove,
	.driver = {
		.name = "ls1021aiot-cpld",
		.of_match_table = ls1021aiot_cpld_of_match,
	},
};
module_platform_driver(ls1021aiot_cpld_driver);

MODULE_AUTHOR("Mateus Lima Alves <mateuslima.ti@gmail.com>");
MODULE_DESCRIPTION("LS1021A-IoT system CPLD LED and DIP switches");
MODULE_LICENSE("GPL v2");
