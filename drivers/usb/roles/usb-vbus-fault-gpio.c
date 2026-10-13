// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO VBUS fault monitor for USB role switches
 *
 * For dual-role ports without a Type-C controller, where host role is
 * selected by policy and the VBUS power switch fault output is the only
 * indication of an over-current or of a second 5V source on VBUS. A fault
 * in host role moves the role switch to device role, which turns VBUS off.
 */

#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/usb/role.h>

struct usb_vbus_fault {
	struct device *dev;
	struct usb_role_switch *role_sw;
	struct gpio_desc *fault_gpiod;
};

static irqreturn_t usb_vbus_fault_isr(int irq, void *data)
{
	struct usb_vbus_fault *vf = data;
	int ret;

	if (usb_role_switch_get_role(vf->role_sw) != USB_ROLE_HOST) {
		dev_warn(vf->dev, "VBUS fault reported outside host role\n");
		return IRQ_HANDLED;
	}

	dev_err(vf->dev, "VBUS fault, leaving host role\n");
	ret = usb_role_switch_set_role(vf->role_sw, USB_ROLE_DEVICE);
	if (ret)
		dev_err(vf->dev, "failed to leave host role: %d\n", ret);

	return IRQ_HANDLED;
}

static void usb_vbus_fault_put_role_sw(void *data)
{
	usb_role_switch_put(data);
}

static int usb_vbus_fault_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct usb_vbus_fault *vf;
	unsigned long flags;
	int irq, ret;

	vf = devm_kzalloc(dev, sizeof(*vf), GFP_KERNEL);
	if (!vf)
		return -ENOMEM;

	vf->dev = dev;

	vf->fault_gpiod = devm_gpiod_get(dev, "fault", GPIOD_IN);
	if (IS_ERR(vf->fault_gpiod))
		return dev_err_probe(dev, PTR_ERR(vf->fault_gpiod),
				     "failed to get fault gpio\n");

	irq = gpiod_to_irq(vf->fault_gpiod);
	if (irq < 0)
		return dev_err_probe(dev, irq, "failed to get fault irq\n");

	vf->role_sw = usb_role_switch_get(dev);
	if (IS_ERR(vf->role_sw))
		return dev_err_probe(dev, PTR_ERR(vf->role_sw),
				     "failed to get role switch\n");
	if (!vf->role_sw)
		return dev_err_probe(dev, -ENODEV, "no role switch\n");

	ret = devm_add_action_or_reset(dev, usb_vbus_fault_put_role_sw,
				       vf->role_sw);
	if (ret)
		return ret;

	/* Fault assertion edge */
	flags = gpiod_is_active_low(vf->fault_gpiod) ?
		IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING;

	ret = devm_request_threaded_irq(dev, irq, NULL, usb_vbus_fault_isr,
					flags | IRQF_ONESHOT, dev_name(dev),
					vf);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request fault irq\n");

	return 0;
}

static const struct of_device_id usb_vbus_fault_of_match[] = {
	{ .compatible = "usb-vbus-fault-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, usb_vbus_fault_of_match);

static struct platform_driver usb_vbus_fault_driver = {
	.probe = usb_vbus_fault_probe,
	.driver = {
		.name = "usb-vbus-fault-gpio",
		.of_match_table = usb_vbus_fault_of_match,
	},
};
module_platform_driver(usb_vbus_fault_driver);

MODULE_DESCRIPTION("GPIO VBUS fault monitor for USB role switches");
MODULE_LICENSE("GPL");
