# NXP NFC PN71XX/SNXXX driver Makefile
nxpnfc-objs = common.o
nxpnfc_i2c-objs = i2c_drv.o
nxpnfc_spi-objs = spi_drv.o

obj-$(CONFIG_NXP_NFC) += nxpnfc.o
obj-$(CONFIG_NXP_NFC_I2C) += nxpnfc_i2c.o
obj-$(CONFIG_NXP_NFC_SPI) += nxpnfc_spi.o

ccflags-$(CONFIG_NXP_NFC_SPI) += -DCONFIG_NXP_NFC_SPI
ccflags-$(CONFIG_NXP_NFC_I2C) += -DCONFIG_NXP_NFC_I2C

#CFLAGS += -DDYNAMIC_DEBUG_MODULE

# Enable dynamic debug for this module when CONFIG_DYNAMIC_DEBUG
ccflags-$(CONFIG_NXP_NFC_SPI) += -DDYNAMIC_DEBUG_MODULE
ccflags-$(CONFIG_NXP_NFC_I2C) += -DDYNAMIC_DEBUG_MODULE
