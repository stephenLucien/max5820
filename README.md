# max5820
kernel driver for maxi,max5820

This is a driver for max5820, a 8 bit DAC. 
This driver is almost the same as driver max5821 in kernel tree.

How to compile this driver?
1. copy this file to kernel drivers/iio/dac/max5820.c
2. modify drivers/iio/dac/Kconfig, and add:
//###################
config MAX5820
	tristate "Maxim MAX5820 DAC driver"
	depends on I2C
	depends on OF
	default m
	help
	  Say yes here to build support for Maxim MAX5820
	  8 bits DAC.
//###################
3. modify drivers/iio/dac/Makefile, and add:
//###################
obj-$(CONFIG_MAX5820) += max5820.o

//###################
4. menuconfig and enable iio, make modules_install  to compile this module.

How to use it?
1. before you can use this module, you should setup this node in device tree.
for example, if there is a max5820 in bus i2c-1, you can add this node to device tree
as following:
&i2c1 {
	// LED DAC
	max5820_led@39 {
		compatible = "maxim,max5820";
		reg = <0x39>;
		vref-supply = <&vccio_3v3_reg>;
	};
}
where vref-supply can be a fixed regulator: 
	vccio_3v3_reg: vccio_3v3_reg {
		compatible = "regulator-fixed";
		regulator-min-microvolt = <3300000>;
		regulator-max-microvolt = <3300000>;
		regulator-name = "vccio_3v3";
		regulator-always-on;
	};
also, if you don't know how to config vref-supply, you can 
discard this property and setup the default vref in max5820.c,
which is defined as DEFAULT_mVREF with preset value 3300.

2. probe this module.
3. access this device in userspace
change directory to this device node, such as /sys/bus/iio/devices/iio:device*/,
this device should has name 'max5820'.
And:
echo 0 > out_voltage0_powerdown to disable powerdown mode of channel 0;
echo value[range from 0 to 255] > out_voltage0_raw to set output voltage.
Vout = Vref * value / 255;




