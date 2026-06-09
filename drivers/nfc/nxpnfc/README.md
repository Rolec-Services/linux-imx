
To verify whether spi0.0 is actually PN7161 node:
1. cat /sys/bus/spi/devices/spi0.0/modalias
2. readlink -f /sys/bus/spi/devices/spi0.0/of_node
3. strings /proc/device-tree/aliases/spi6
