// SPDX-License-Identifier: GPL-2.0-only
/*
 * Microchip (Atmel) AT42QT2120 QTouch 12-key capacitive sensor
 *
 * The chip reports key state in two status bytes and pulls its open-drain
 * CHANGE line low until the host has read the status registers. Each key
 * channel is mapped to an input keycode through "linux,keycodes"; channels
 * mapped to KEY_RESERVED (e.g. guard or proximity electrodes) are ignored.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/property.h>

#define QT2120_REG_CHIP_ID		0x00
#define QT2120_CHIP_ID			0x3e
#define QT2120_REG_FW_VERSION		0x01
#define QT2120_REG_DET_STATUS		0x02
#define QT2120_REG_KEY_STATUS		0x03	/* 0x03: keys 0-7, 0x04: keys 8-11 */
#define QT2120_REG_CALIBRATE		0x06
#define QT2120_REG_RESET		0x07
#define QT2120_REG_DTHR(n)		(0x10 + (n))
#define QT2120_REG_KEY_CTRL(n)		(0x1c + (n))

#define QT2120_DET_CALIBRATE		BIT(7)

#define QT2120_NUM_KEYS			12
#define QT2120_KEY_MASK			GENMASK(QT2120_NUM_KEYS - 1, 0)
#define QT2120_RESET_TIME_MS		200
#define QT2120_CAL_TIMEOUT_MS		1000

struct qt2120 {
	struct i2c_client *client;
	struct input_dev *input;
	unsigned short keycodes[QT2120_NUM_KEYS];
	u16 last_keys;
};

/* Reading the status block from DET_STATUS onwards releases CHANGE */
static int qt2120_read_status(struct qt2120 *qt, u8 *det, u16 *keys)
{
	u8 buf[4];
	int ret;

	ret = i2c_smbus_read_i2c_block_data(qt->client, QT2120_REG_DET_STATUS,
					    sizeof(buf), buf);
	if (ret < 0)
		return ret;
	if (ret != sizeof(buf))
		return -EIO;

	*det = buf[0];
	*keys = (buf[1] | (buf[2] << 8)) & QT2120_KEY_MASK;
	return 0;
}

static irqreturn_t qt2120_irq(int irq, void *dev_id)
{
	struct qt2120 *qt = dev_id;
	unsigned long changed;
	u16 keys;
	u8 det;
	int i;

	if (qt2120_read_status(qt, &det, &keys)) {
		dev_err_ratelimited(&qt->client->dev, "failed to read status\n");
		return IRQ_HANDLED;
	}

	changed = keys ^ qt->last_keys;
	if (!changed)
		return IRQ_HANDLED;

	for_each_set_bit(i, &changed, QT2120_NUM_KEYS) {
		if (qt->keycodes[i] == KEY_RESERVED)
			continue;
		input_report_key(qt->input, qt->keycodes[i], keys & BIT(i));
	}
	input_sync(qt->input);
	qt->last_keys = keys;

	return IRQ_HANDLED;
}

static int qt2120_write_regs(struct qt2120 *qt, const char *prop,
			     unsigned int (*reg)(unsigned int))
{
	struct device *dev = &qt->client->dev;
	u8 vals[QT2120_NUM_KEYS];
	int i, ret;

	if (!device_property_present(dev, prop))
		return 0;

	ret = device_property_read_u8_array(dev, prop, vals, ARRAY_SIZE(vals));
	if (ret)
		return dev_err_probe(dev, ret, "%s needs %d entries\n",
				     prop, QT2120_NUM_KEYS);

	for (i = 0; i < QT2120_NUM_KEYS; i++) {
		ret = i2c_smbus_write_byte_data(qt->client, reg(i), vals[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static unsigned int qt2120_dthr_reg(unsigned int n)
{
	return QT2120_REG_DTHR(n);
}

static unsigned int qt2120_key_ctrl_reg(unsigned int n)
{
	return QT2120_REG_KEY_CTRL(n);
}

static int qt2120_calibrate(struct qt2120 *qt)
{
	unsigned long timeout;
	u16 keys;
	u8 det;
	int ret;

	ret = i2c_smbus_write_byte_data(qt->client, QT2120_REG_CALIBRATE, 1);
	if (ret)
		return ret;

	timeout = jiffies + msecs_to_jiffies(QT2120_CAL_TIMEOUT_MS);
	do {
		msleep(20);
		ret = qt2120_read_status(qt, &det, &keys);
		if (ret)
			return ret;
		if (!(det & QT2120_DET_CALIBRATE))
			return 0;
	} while (time_before(jiffies, timeout));

	return -ETIMEDOUT;
}

static int qt2120_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gpio_desc *reset;
	struct qt2120 *qt;
	u32 codes[QT2120_NUM_KEYS];
	int nkeys, ret, i;
	u16 keys;
	u8 det;

	if (!client->irq)
		return dev_err_probe(dev, -EINVAL, "no interrupt\n");

	qt = devm_kzalloc(dev, sizeof(*qt), GFP_KERNEL);
	if (!qt)
		return -ENOMEM;
	qt->client = client;

	/* nRESET is held low by a board pull-down until we release it */
	reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "failed to get reset gpio\n");
	if (reset) {
		usleep_range(1000, 2000);
		gpiod_set_value_cansleep(reset, 0);
		msleep(QT2120_RESET_TIME_MS);
	}

	ret = i2c_smbus_read_byte_data(client, QT2120_REG_CHIP_ID);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to read chip id\n");
	if (ret != QT2120_CHIP_ID)
		return dev_err_probe(dev, -ENODEV, "unexpected chip id 0x%02x\n", ret);

	ret = i2c_smbus_read_byte_data(client, QT2120_REG_FW_VERSION);
	if (ret >= 0)
		dev_info(dev, "AT42QT2120 firmware %d.%d\n", ret >> 4, ret & 0xf);

	nkeys = device_property_count_u32(dev, "linux,keycodes");
	if (nkeys <= 0 || nkeys > QT2120_NUM_KEYS)
		return dev_err_probe(dev, -EINVAL,
				     "linux,keycodes must have 1..%d entries\n",
				     QT2120_NUM_KEYS);
	ret = device_property_read_u32_array(dev, "linux,keycodes", codes, nkeys);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read linux,keycodes\n");

	ret = qt2120_write_regs(qt, "microchip,detect-threshold", qt2120_dthr_reg);
	if (ret)
		return ret;
	ret = qt2120_write_regs(qt, "microchip,key-control", qt2120_key_ctrl_reg);
	if (ret)
		return ret;

	ret = qt2120_calibrate(qt);
	if (ret)
		dev_warn(dev, "calibration did not complete: %d\n", ret);

	qt->input = devm_input_allocate_device(dev);
	if (!qt->input)
		return -ENOMEM;

	qt->input->name = "AT42QT2120 Touch Buttons";
	qt->input->id.bustype = BUS_I2C;
	qt->input->keycode = qt->keycodes;
	qt->input->keycodesize = sizeof(qt->keycodes[0]);
	qt->input->keycodemax = QT2120_NUM_KEYS;

	for (i = 0; i < nkeys; i++) {
		qt->keycodes[i] = codes[i];
		if (codes[i] != KEY_RESERVED)
			input_set_capability(qt->input, EV_KEY, codes[i]);
	}

	ret = input_register_device(qt->input);
	if (ret)
		return ret;

	/* Release CHANGE and take the initial state as the baseline */
	ret = qt2120_read_status(qt, &det, &keys);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read status\n");
	qt->last_keys = keys;

	ret = devm_request_threaded_irq(dev, client->irq, NULL, qt2120_irq,
					IRQF_ONESHOT, dev_name(dev), qt);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	i2c_set_clientdata(client, qt);
	return 0;
}

static int qt2120_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	disable_irq(client->irq);
	return 0;
}

static int qt2120_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	enable_irq(client->irq);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(qt2120_pm_ops, qt2120_suspend, qt2120_resume);

static const struct i2c_device_id qt2120_id[] = {
	{ "at42qt2120" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, qt2120_id);

static const struct of_device_id qt2120_of_match[] = {
	{ .compatible = "microchip,at42qt2120" },
	{ }
};
MODULE_DEVICE_TABLE(of, qt2120_of_match);

static struct i2c_driver qt2120_driver = {
	.driver = {
		.name = "qt2120",
		.of_match_table = qt2120_of_match,
		.pm = pm_sleep_ptr(&qt2120_pm_ops),
	},
	.probe = qt2120_probe,
	.id_table = qt2120_id,
};
module_i2c_driver(qt2120_driver);

MODULE_DESCRIPTION("Microchip AT42QT2120 capacitive touch key driver");
MODULE_LICENSE("GPL");
