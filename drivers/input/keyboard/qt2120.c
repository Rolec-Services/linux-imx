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
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/slab.h>

#define QT2120_REG_CHIP_ID		0x00
#define QT2120_CHIP_ID			0x3e
#define QT2120_REG_FW_VERSION		0x01
#define QT2120_REG_DET_STATUS		0x02
#define QT2120_REG_KEY_STATUS		0x03	/* 0x03: keys 0-7, 0x04: keys 8-11 */
#define QT2120_REG_CALIBRATE		0x06
#define QT2120_REG_RESET		0x07
#define QT2120_REG_DTHR(n)		(0x10 + (n))
#define QT2120_REG_KEY_CTRL(n)		(0x1c + (n))
#define QT2120_REG_PULSE_SCALE(n)	(0x28 + (n))
/* 12 x 16-bit, LSB first (the datasheet table says MSB, the chip disagrees) */
#define QT2120_REG_KEY_SIGNAL		0x34
#define QT2120_REG_REFERENCE		0x4c

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
	struct mutex lock;	/* serialises sysfs reconfiguration */
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

static void qt2120_process(struct qt2120 *qt)
{
	unsigned long changed;
	u16 keys;
	u8 det;
	int i;

	if (qt2120_read_status(qt, &det, &keys)) {
		dev_err_ratelimited(&qt->client->dev, "failed to read status\n");
		return;
	}

	changed = keys ^ qt->last_keys;
	if (!changed)
		return;

	for_each_set_bit(i, &changed, QT2120_NUM_KEYS) {
		if (qt->keycodes[i] == KEY_RESERVED)
			continue;
		input_report_key(qt->input, qt->keycodes[i], keys & BIT(i));
	}
	input_sync(qt->input);
	qt->last_keys = keys;
}

static irqreturn_t qt2120_irq(int irq, void *dev_id)
{
	qt2120_process(dev_id);
	return IRQ_HANDLED;
}

/* Write one byte per key to the per-key register block starting at @reg */
static int qt2120_write_keys(struct qt2120 *qt, u8 reg, const u8 *vals)
{
	int i, ret;

	for (i = 0; i < QT2120_NUM_KEYS; i++) {
		ret = i2c_smbus_write_byte_data(qt->client, reg + i, vals[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int qt2120_write_prop(struct qt2120 *qt, const char *prop, u8 reg)
{
	struct device *dev = &qt->client->dev;
	u8 vals[QT2120_NUM_KEYS];
	int ret;

	if (!device_property_present(dev, prop))
		return 0;

	ret = device_property_read_u8_array(dev, prop, vals, ARRAY_SIZE(vals));
	if (ret)
		return dev_err_probe(dev, ret, "%s needs %d entries\n",
				     prop, QT2120_NUM_KEYS);

	return qt2120_write_keys(qt, reg, vals);
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

static int qt2120_read_words(struct qt2120 *qt, u8 reg, u16 *vals)
{
	u8 buf[QT2120_NUM_KEYS * 2];
	int ret, i;

	ret = i2c_smbus_read_i2c_block_data(qt->client, reg, sizeof(buf), buf);
	if (ret < 0)
		return ret;
	if (ret != sizeof(buf))
		return -EIO;

	for (i = 0; i < QT2120_NUM_KEYS; i++)
		vals[i] = buf[2 * i] | (buf[2 * i + 1] << 8);
	return 0;
}

/* Raw acquisition data for tuning thresholds and oversampling */
static ssize_t key_data_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct qt2120 *qt = dev_get_drvdata(dev);
	u16 sig[QT2120_NUM_KEYS], ref[QT2120_NUM_KEYS];
	int len = 0, ret, i;

	ret = qt2120_read_words(qt, QT2120_REG_KEY_SIGNAL, sig);
	if (ret)
		return ret;
	ret = qt2120_read_words(qt, QT2120_REG_REFERENCE, ref);
	if (ret)
		return ret;

	for (i = 0; i < QT2120_NUM_KEYS; i++)
		len += sysfs_emit_at(buf, len, "KEY%-2d signal %5u reference %5u delta %6d\n",
				     i, sig[i], ref[i], (int)sig[i] - (int)ref[i]);
	return len;
}
static DEVICE_ATTR_RO(key_data);

/*
 * Apply new settings at runtime: CHANGE is masked while the registers are
 * written and the chip recalibrates, then key state is resynchronised.
 */
static int qt2120_reconfigure(struct qt2120 *qt, u8 reg, const u8 *vals)
{
	int ret = 0;

	mutex_lock(&qt->lock);
	disable_irq(qt->client->irq);

	if (vals)
		ret = qt2120_write_keys(qt, reg, vals);
	if (!ret)
		ret = qt2120_calibrate(qt);
	qt2120_process(qt);

	enable_irq(qt->client->irq);
	mutex_unlock(&qt->lock);

	return ret;
}

/* Tuning registers: one value per key, KEY0 first */
struct qt2120_reg_attr {
	struct device_attribute attr;
	u8 reg;
};

static ssize_t qt2120_regs_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct qt2120 *qt = dev_get_drvdata(dev);
	u8 reg = container_of(attr, struct qt2120_reg_attr, attr)->reg;
	u8 vals[QT2120_NUM_KEYS];
	int len = 0, ret, i;

	ret = i2c_smbus_read_i2c_block_data(qt->client, reg, sizeof(vals), vals);
	if (ret < 0)
		return ret;
	if (ret != sizeof(vals))
		return -EIO;

	for (i = 0; i < QT2120_NUM_KEYS; i++)
		len += sysfs_emit_at(buf, len, "0x%02x%c", vals[i],
				     i == QT2120_NUM_KEYS - 1 ? '\n' : ' ');
	return len;
}

static ssize_t qt2120_regs_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct qt2120 *qt = dev_get_drvdata(dev);
	u8 reg = container_of(attr, struct qt2120_reg_attr, attr)->reg;
	u8 vals[QT2120_NUM_KEYS];
	char *str, *p, *tok;
	int n = 0, ret = 0;

	str = kstrndup(buf, count, GFP_KERNEL);
	if (!str)
		return -ENOMEM;

	p = str;
	while ((tok = strsep(&p, " \t\n"))) {
		if (!*tok)
			continue;
		if (n == QT2120_NUM_KEYS) {
			ret = -EINVAL;
			break;
		}
		ret = kstrtou8(tok, 0, &vals[n++]);
		if (ret)
			break;
	}
	kfree(str);

	if (!ret && n != QT2120_NUM_KEYS)
		ret = -EINVAL;
	if (!ret)
		ret = qt2120_reconfigure(qt, reg, vals);

	return ret ?: count;
}

#define QT2120_REG_ATTR(_name, _reg)					\
	static struct qt2120_reg_attr dev_attr_##_name = {		\
		.attr = __ATTR(_name, 0644, qt2120_regs_show,		\
			       qt2120_regs_store),			\
		.reg = _reg,						\
	}

QT2120_REG_ATTR(detect_threshold, QT2120_REG_DTHR(0));
QT2120_REG_ATTR(key_control, QT2120_REG_KEY_CTRL(0));
QT2120_REG_ATTR(pulse_scale, QT2120_REG_PULSE_SCALE(0));

static ssize_t calibrate_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	int ret = qt2120_reconfigure(dev_get_drvdata(dev), 0, NULL);

	return ret ?: count;
}
static DEVICE_ATTR_WO(calibrate);

static struct attribute *qt2120_attrs[] = {
	&dev_attr_key_data.attr,
	&dev_attr_detect_threshold.attr.attr,
	&dev_attr_key_control.attr.attr,
	&dev_attr_pulse_scale.attr.attr,
	&dev_attr_calibrate.attr,
	NULL
};
ATTRIBUTE_GROUPS(qt2120);

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
	mutex_init(&qt->lock);
	i2c_set_clientdata(client, qt);

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

	ret = qt2120_write_prop(qt, "microchip,detect-threshold", QT2120_REG_DTHR(0));
	if (ret)
		return ret;
	ret = qt2120_write_prop(qt, "microchip,key-control", QT2120_REG_KEY_CTRL(0));
	if (ret)
		return ret;
	ret = qt2120_write_prop(qt, "microchip,pulse-scale", QT2120_REG_PULSE_SCALE(0));
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
		.dev_groups = qt2120_groups,
		.pm = pm_sleep_ptr(&qt2120_pm_ops),
	},
	.probe = qt2120_probe,
	.id_table = qt2120_id,
};
module_i2c_driver(qt2120_driver);

MODULE_DESCRIPTION("Microchip AT42QT2120 capacitive touch key driver");
MODULE_LICENSE("GPL");
