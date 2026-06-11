
Load command:
1. SPI variant: sudo modprobe nxpnfc_spi
2. I2C variant: sudo modprobe nxpnfc_i2c


Use dynamic debug instead:
1. sudo modprobe nxpnfc_spi dyndbg=+p
2. sudo modprobe nxpnfc dyndbg=+p

Or after load:
1. sudo mount -t debugfs none /sys/kernel/debug
2. echo 'module nxpnfc +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
3. echo 'module nxpnfc_spi +p' | sudo tee /sys/kernel/debug/dynamic_debug/control

If by dbg=7 you meant printk debug verbosity:
1. sudo dmesg -n 8
2. or: echo 8 | sudo tee /proc/sys/kernel/printk

This ensures pr_debug messages are allowed to reach console/logs when dynamic debug is enabled.
