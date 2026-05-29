/******************************************************************************
 * Copyright (C) 2015, The Linux Foundation. All rights reserved.
 * Copyright 2019-2021,2023 NXP
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 ******************************************************************************/
#ifndef _SPI_DRV_H_
#define _SPI_DRV_H_
#include <linux/spi/spi.h>

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/of.h>

#include <linux/miscdevice.h>
#include <linux/spinlock.h>

/* Forward declaration — full definition is in common.h */
struct nfc_dev;

/*kept same as dts */
#define NFC_DRV_STR			"nxp,nxpnfc"
#define NFC_DEV_ID			"nxpnfc"
#define WRITE_PREFIX_ON_WRITE		(0x7F)
#define WRITE_PREFIX_ON_READ		(0xFF)
#define PREFIX_LENGTH			1
#define PREFIX_INDEX			0
#define MISO_VAL_ON_WRITE_SUCCESS	(0xFF)

/* Interface specific parameters */
struct spi_dev {
	struct spi_device *client;
	struct miscdevice   device;
	/* IRQ parameters */
	bool irq_enabled;
	spinlock_t irq_enabled_lock;
	/* NFC_IRQ wake-up state */
	bool irq_wake_up;
	/* Temparary write kernel buffer used for spi_write_then_read() API to write 0xFF byte*/
	uint8_t *tmp_write_kbuf;
	/* Temparary read kernel buffer used for spi_sync() API to write 0x7F byte as a prefix for the command*/
	uint8_t *tmp_read_kbuf;
};

int nfc_spi_dev_probe(struct spi_device *client);
void nfc_spi_dev_remove(struct spi_device *client);
int nfc_spi_dev_suspend(struct device *device);
int nfc_spi_dev_resume(struct device *device);
uint8_t *add_spi_write_prefix(uint8_t *cmd);
uint8_t *add_write_prefix_for_spi_read(uint8_t *cmd);
int spi_disable_irq(struct nfc_dev *dev);
int spi_enable_irq(struct nfc_dev *dev);
int get_spi_command_length(int actual_length);
int nfc_spi_read(struct nfc_dev *nfc_dev, char *buf, size_t count, int timeout);
int nfc_spi_write(struct nfc_dev *nfc_dev, const char *buf, size_t count, int max_retry_cnt);
ssize_t nfc_spi_dev_read(struct file *filp, char __user *buf, size_t count,
			 loff_t *offset);
ssize_t nfc_spi_dev_write(struct file *filp, const char __user *buf,
			  size_t count, loff_t *offset);

#endif //_SPI_DRV_H_
