 /*
  * iio/dac/max5820.c
  * Copyright (C) 2014 Philippe Reynes
  *
  * This program is free software; you can redistribute it and/or modify
  * it under the terms of the GNU General Public License version 2 as
  * published by the Free Software Foundation.
  */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/regulator/consumer.h>

#define DEFAULT_mVREF (3300)

#define MAX5820_MAX_DAC_CHANNELS		2

/* command bytes */
#define MAX5820_LOAD_DAC_A_IN_REG_B		0x00
#define MAX5820_LOAD_DAC_B_IN_REG_A		0x10
#define MAX5820_EXTENDED_COMMAND_MODE		0xf0
#define MAX5820_READ_DAC_A_COMMAND		0xf1
#define MAX5820_READ_DAC_B_COMMAND		0xf2

#define MAX5820_EXTENDED_POWER_UP		0x00
#define MAX5820_EXTENDED_POWER_DOWN_MODE0	0x01
#define MAX5820_EXTENDED_POWER_DOWN_MODE1	0x02
#define MAX5820_EXTENDED_POWER_DOWN_MODE2	0x03
#define MAX5820_EXTENDED_DAC_A			0x04
#define MAX5820_EXTENDED_DAC_B			0x08

enum max5820_device_ids {
        ID_MAX5820,
};

struct max5820_data {
	struct i2c_client	*client;
	struct regulator	*vref_reg;
	unsigned short		vref_mv;
        bool			powerdown[MAX5820_MAX_DAC_CHANNELS];
        u8			powerdown_mode[MAX5820_MAX_DAC_CHANNELS];
	struct mutex		lock;
};

static const char * const max5820_powerdown_modes[] = {
	"three_state",
	"1kohm_to_gnd",
	"100kohm_to_gnd",
};

enum {
        MAX5820_THREE_STATE,
        MAX5820_1KOHM_TO_GND,
        MAX5820_100KOHM_TO_GND
};

static int max5820_get_powerdown_mode(struct iio_dev *indio_dev,
				      const struct iio_chan_spec *chan)
{
        struct max5820_data *st = iio_priv(indio_dev);

	return st->powerdown_mode[chan->channel];
}

static int max5820_set_powerdown_mode(struct iio_dev *indio_dev,
				      const struct iio_chan_spec *chan,
				      unsigned int mode)
{
        struct max5820_data *st = iio_priv(indio_dev);

	st->powerdown_mode[chan->channel] = mode;

	return 0;
}

static const struct iio_enum max5820_powerdown_mode_enum = {
        .items = max5820_powerdown_modes,
        .num_items = ARRAY_SIZE(max5820_powerdown_modes),
        .get = max5820_get_powerdown_mode,
        .set = max5820_set_powerdown_mode,
};

static ssize_t max5820_read_dac_powerdown(struct iio_dev *indio_dev,
					  uintptr_t private,
					  const struct iio_chan_spec *chan,
					  char *buf)
{
        struct max5820_data *st = iio_priv(indio_dev);

	return sprintf(buf, "%d\n", st->powerdown[chan->channel]);
}

static int max5820_sync_powerdown_mode(struct max5820_data *data,
				       const struct iio_chan_spec *chan)
{
	u8 outbuf[2];

        outbuf[0] = MAX5820_EXTENDED_COMMAND_MODE;

	if (chan->channel == 0)
                outbuf[1] = MAX5820_EXTENDED_DAC_A;
	else
                outbuf[1] = MAX5820_EXTENDED_DAC_B;

	if (data->powerdown[chan->channel])
		outbuf[1] |= data->powerdown_mode[chan->channel] + 1;
	else
                outbuf[1] |= MAX5820_EXTENDED_POWER_UP;

	return i2c_master_send(data->client, outbuf, 2);
}

static ssize_t max5820_write_dac_powerdown(struct iio_dev *indio_dev,
					   uintptr_t private,
					   const struct iio_chan_spec *chan,
					   const char *buf, size_t len)
{
        struct max5820_data *data = iio_priv(indio_dev);
	bool powerdown;
	int ret;

	ret = strtobool(buf, &powerdown);
	if (ret)
		return ret;

	data->powerdown[chan->channel] = powerdown;

        ret = max5820_sync_powerdown_mode(data, chan);
	if (ret < 0)
		return ret;

	return len;
}

static const struct iio_chan_spec_ext_info max5820_ext_info[] = {
	{
		.name = "powerdown",
                .read = max5820_read_dac_powerdown,
                .write = max5820_write_dac_powerdown,
		.shared = IIO_SEPARATE,
	},
        IIO_ENUM("powerdown_mode", IIO_SEPARATE, &max5820_powerdown_mode_enum),
        IIO_ENUM_AVAILABLE("powerdown_mode", &max5820_powerdown_mode_enum),
	{ },
};

#define MAX5820_CHANNEL(chan) {					\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.output = 1,						\
	.channel = (chan),					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SCALE),	\
        .ext_info = max5820_ext_info,				\
}

static const struct iio_chan_spec max5820_channels[] = {
        MAX5820_CHANNEL(0),
        MAX5820_CHANNEL(1)
};

static const u8 max5820_read_dac_command[] = {
        MAX5820_READ_DAC_A_COMMAND,
        MAX5820_READ_DAC_B_COMMAND
};

static const u8 max5820_load_dac_command[] = {
        MAX5820_LOAD_DAC_A_IN_REG_B,
        MAX5820_LOAD_DAC_B_IN_REG_A
};

static int max5820_get_value(struct iio_dev *indio_dev,
			     int *val, int channel)
{
        struct max5820_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;
	u8 outbuf[1];
	u8 inbuf[2];
	int ret;

	if ((channel != 0) && (channel != 1))
		return -EINVAL;

        outbuf[0] = max5820_read_dac_command[channel];

	mutex_lock(&data->lock);

	ret = i2c_master_send(client, outbuf, 1);
	if (ret < 0) {
		mutex_unlock(&data->lock);
		return ret;
	} else if (ret != 1) {
		mutex_unlock(&data->lock);
		return -EIO;
	}

	ret = i2c_master_recv(client, inbuf, 2);
	if (ret < 0) {
		mutex_unlock(&data->lock);
		return ret;
	} else if (ret != 2) {
		mutex_unlock(&data->lock);
		return -EIO;
	}

	mutex_unlock(&data->lock);

        *val = ((inbuf[0] & 0x0f) << 4) | (inbuf[1] >> 4);

	return IIO_VAL_INT;
}

static int max5820_set_value(struct iio_dev *indio_dev,
			     int val, int channel)
{
        struct max5820_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;
	u8 outbuf[2];
	int ret;

        if ((val < 0) || (val > 255))
		return -EINVAL;

	if ((channel != 0) && (channel != 1))
		return -EINVAL;

        outbuf[0] = max5820_load_dac_command[channel];
        outbuf[0] |= (val >> 4);
        outbuf[1] =  ( (val & 0x0f) << 4 );

	ret = i2c_master_send(client, outbuf, 2);
	if (ret < 0)
		return ret;
	else if (ret != 2)
		return -EIO;
	else
		return 0;
}

static int max5820_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
        struct max5820_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
                return max5820_get_value(indio_dev, val, chan->channel);
	case IIO_CHAN_INFO_SCALE:
		*val = data->vref_mv;
		*val2 = 10;
		return IIO_VAL_FRACTIONAL_LOG2;
	default:
		return -EINVAL;
	}
}

static int max5820_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	if (val2 != 0)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
                return max5820_set_value(indio_dev, val, chan->channel);
	default:
		return -EINVAL;
	}
}

#ifdef CONFIG_PM_SLEEP
static int max5820_suspend(struct device *dev)
{
        u8 outbuf[2] = { MAX5820_EXTENDED_COMMAND_MODE,
                         MAX5820_EXTENDED_DAC_A |
                         MAX5820_EXTENDED_DAC_B |
                         MAX5820_EXTENDED_POWER_DOWN_MODE2 };

	return i2c_master_send(to_i2c_client(dev), outbuf, 2);
}

static int max5820_resume(struct device *dev)
{
        u8 outbuf[2] = { MAX5820_EXTENDED_COMMAND_MODE,
                         MAX5820_EXTENDED_DAC_A |
                         MAX5820_EXTENDED_DAC_B |
                         MAX5820_EXTENDED_POWER_UP };

	return i2c_master_send(to_i2c_client(dev), outbuf, 2);
}

static SIMPLE_DEV_PM_OPS(max5820_pm_ops, max5820_suspend, max5820_resume);
#define MAX5820_PM_OPS (&max5820_pm_ops)
#else
#define MAX5820_PM_OPS NULL
#endif /* CONFIG_PM_SLEEP */

static const struct iio_info max5820_info = {
        .read_raw = max5820_read_raw,
        .write_raw = max5820_write_raw,
	.driver_module = THIS_MODULE,
};

static int max5820_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
        struct max5820_data *data;
	struct iio_dev *indio_dev;
	u32 tmp;
	int ret;

	printk("%s\n",__FUNCTION__);

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev){
		printk("error: max5820 devm_iio_device_alloc.\n");
		return -ENOMEM;
	}
	data = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	data->client = client;
	mutex_init(&data->lock);

        /* max5820 start in powerdown mode 100Kohm to ground */
        for (tmp = 0; tmp < MAX5820_MAX_DAC_CHANNELS; tmp++) {
		data->powerdown[tmp] = true;
                data->powerdown_mode[tmp] = MAX5820_100KOHM_TO_GND;
	}

	data->vref_reg = devm_regulator_get(&client->dev, "vref");

	if (IS_ERR(data->vref_reg)) {

		ret = PTR_ERR(data->vref_reg);
#ifndef DEFAULT_mVREF
		dev_err(&client->dev, "Failed to get vref regulator: %d\n", ret);
                goto error_free_reg;
#else
		printk("%s: Failed to get vref regulator: %d\n", __func__, ret);
                goto setup_default_vref;
#endif
	}
        else{
                ret = regulator_enable(data->vref_reg);
                if (ret) {
			regulator_disable(data->vref_reg);
#ifndef DEFAULT_mVREF
                        dev_err(&client->dev,
                            "Failed to enable vref regulator: %d\n", ret);                   
                        goto error_free_reg;
#else
                        goto setup_default_vref;
#endif
                }

                ret = regulator_get_voltage(data->vref_reg);
                if (ret < 0) {
			regulator_disable(data->vref_reg);
#ifndef DEFAULT_mVREF
                        dev_err(&client->dev,
                            "Failed to get voltage on regulator: %d\n", ret);                       
                        goto error_free_reg;
#else
			printk("%s: Failed to get voltage on regulator: %d\n", __func__, ret);
                        goto setup_default_vref;
#endif
                }
        }


	data->vref_mv = ret / 1000;

#ifdef DEFAULT_mVREF
        if(0){
setup_default_vref:
		
            	data->vref_reg= NULL;
            	data->vref_mv = DEFAULT_mVREF;
        }
#endif
	printk("%s: max5820 set Vref(mV)=%d.\n",__func__,data->vref_mv);
	indio_dev->name = id->name;
	indio_dev->dev.parent = &client->dev;
        indio_dev->num_channels = ARRAY_SIZE(max5820_channels);
        indio_dev->channels = max5820_channels;
	indio_dev->modes = INDIO_DIRECT_MODE;
        indio_dev->info = &max5820_info;

	printk("usage: \ncd to this device node, /sys/bus/iio/devices/iio:device*/, which has member 'name' with value 'max5820'.\n cat member 'scale' to get Vref(V).\n echo <0|1 : 0 means enable voltage output, while 1 means powerdown> > out_voltage*_powerdown \n echo <var:0-255> > out_voltage0_raw\n and the output voltage will be (var/255*Vref) Volt.\n");
	return iio_device_register(indio_dev);
}

static int max5820_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
        struct max5820_data *data = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
        if(data->vref_reg) regulator_disable(data->vref_reg);

	return 0;
}

static const struct i2c_device_id max5820_id[] = {
        { "max5820", ID_MAX5820 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max5820_id);

static const struct of_device_id max5820_of_match[] = {
        { .compatible = "maxim,max5820" },
	{ }
};
MODULE_DEVICE_TABLE(of, max5820_of_match);

static struct i2c_driver max5820_driver = {
	.driver = {
                .name	= "max5820",
                .pm     = MAX5820_PM_OPS,
	},
        .probe		= max5820_probe,
        .remove		= max5820_remove,
        .id_table	= max5820_id,
};
module_i2c_driver(max5820_driver);

MODULE_AUTHOR("Philippe Reynes <tremyfr@yahoo.fr>");
MODULE_DESCRIPTION("MAX5820 DAC");
MODULE_LICENSE("GPL v2");
