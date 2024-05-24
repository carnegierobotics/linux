/*
 * sound/soc/ambarella/ak7719.c
 *
  * History:
 *	2014/04/17 - [Ken He] Created file
 *
 * Copyright (C) 2014-2018, Ambarella, Inc.
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <plat/audio.h>
#include "ak7719_dsp.h"

//#define AK7719_DEBUG

#ifdef AK7719_DEBUG
#define akdbgprt printk
#else
#define akdbgprt(format, arg...) do {} while (0)
#endif

static int crc_timeout = 10;
module_param(crc_timeout, uint, 0664);

static int aec = 1;
module_param(aec, uint, 0664);

static int ak7719_read_reg(struct ak7719_data *ak7719, u8 reg)
{
	int ret = i2c_smbus_read_byte_data(ak7719->client, reg);
	if (ret > 0)
		ret &= 0xff;

	return ret;
}

static int ak7719_write_reg(struct ak7719_data *ak7719,
		u8 reg, u8 value)
{
	return i2c_smbus_write_byte_data(ak7719->client, reg, value);
}

/*used to calculate the crc value for PRAM or CRAM binary*/
unsigned short calc_crc(const char *str, unsigned short length)
{
	unsigned short crc = 0x0000;
	int i, j;
	for(i = 0; i < length; i++){
		crc ^= *str++ << 8;
		for(j = 0; j < 8; j++){
			if(crc & 0x8000){
				crc <<= 1;
				crc ^= CRC16_CCITT;
			} else {
				crc <<= 1;
			}
		}
	}

	return crc;
}

static int ak7719_write_command(struct ak7719_data *ak7719, u8 cmdvalue,
		u32 subaddr, u8 *pbuf, unsigned int length, u16 *crc_val)
{
	int ret;
	u8 *bufcmd = NULL;
	struct i2c_msg msg[1];
	struct i2c_client *client;
	client = ak7719->client;

	bufcmd = kmalloc(length+3, GFP_KERNEL);
	if (!bufcmd) {
		return -ENOMEM;
	}
	bufcmd[0] = cmdvalue;
	bufcmd[1] = (subaddr & 0xff00) >> 8;
	bufcmd[2] = subaddr & 0xff;
	memcpy(bufcmd + 3, pbuf, length);

	/*calculate the crc value for the writing data*/
	if(*crc_val == 0)
		*crc_val = calc_crc(bufcmd, length+3);

	/*begin to encapsulate the message for i2c*/
	msg[0].addr = client->addr;
	msg[0].len = length +3;
	msg[0].flags = 0;
	msg[0].buf = bufcmd;

	ret = i2c_transfer(client->adapter, msg, 1);
	udelay(100);

	kfree(bufcmd);
	if (ret < 0) {
		dev_err(&client->dev, "Failed writing cmd 0x%02x!\n", cmdvalue);
		return ret;
	}
	return 0;
}

unsigned short ak7719_ram_read(struct ak7719_data *ak7719, char *sbuf, u8 slen, char *rbuf, u8 rlen){
	int ret;

	ret = i2c_smbus_read_i2c_block_data(ak7719->client, sbuf[0], rlen, rbuf);

	return 0;
}

static int ak7719_ram_download_crc(struct ak7719_data *ak7719, u8 cmd, u8 *data, int len ,u16 crc_value)
{
	u16 crc_val = crc_value;
	u16 crc_flag = 1;
	char crc_tx[1] = { 0x72 };
	char crc_rx[2];
	int ret, ret1, timeout = crc_timeout; //timeout = 3; chane 3 times to 10 times

	do {	/*write PRAM or CRAM and get crc value*/
		ret1 = ak7719_write_command(ak7719, cmd, 0, data, len, &crc_val);
		if (ret1 != 0) {
			pr_err(" write ram error in function  %s\n ",__FUNCTION__);
			continue;
		}

		/*get the crc value by command 0x72*/
		 do {
			ret = ak7719_ram_read(ak7719, crc_tx, 1, crc_rx, 2);
			if(ret != 0)
				pr_err("%s:  read crc err\n",__FUNCTION__);
		 } while (ret != 0);

		ret  =  (crc_rx[0] << 8) + crc_rx[1];
		akdbgprt("pram : crc flag ret=%x \n" ,ret);
		crc_flag  =  (crc_val == ret);
		timeout--;
		udelay(10);
	} while (crc_flag == 0 && timeout > 0);

	akdbgprt("crc_flag = %d timeout = %d\n", crc_flag, timeout);
	if(timeout == 0)
		return -1;
	return 0;
}

static void ak7719_download_dsp_pro(struct ak7719_data *ak7719)
{
	int ret;

	if(aec == 1) {
		/* pram data is ak7719 dsp binary */
		ret = ak7719_ram_download_crc(ak7719, 0xB8, pram_data, ARRAY_SIZE(pram_data), 0xc4a9);
		if(ret < 0) {
			printk("ak7719:write dsp binary failed!!!\n");
		}
	} else if(aec == 0) {
		/* ak77dspPRAM is ak7719 bypass dsp binary */
		ak7719_ram_download_crc(ak7719, 0xB8, ak77dspPRAM, ARRAY_SIZE(ak77dspPRAM), 0x5bf4);
	}
	ret = ak7719_ram_download_crc(ak7719, 0xB4, cram_data, ARRAY_SIZE(cram_data), 0x50df);
	if(ret < 0)
		printk("ak7719:write cram data failed\n");

	ak7719_write_reg(ak7719, 0xc6, 0x0);
	ak7719_write_reg(ak7719, 0xc6, 0x4);

}

static void ak7719_download_work(struct work_struct *work){
	struct ak7719_workqueue *ak7719_work = container_of(work,struct ak7719_workqueue, download_binary);
	ak7719_download_dsp_pro(ak7719_work->data);
}

#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)
static int ak7719_i2c_probe(struct i2c_client *i2c,
			    const struct i2c_device_id *id)
{
	int rval = 0;
	enum of_gpio_flags flags;
	struct device_node *np = i2c->dev.of_node;
	struct ak7719_data *ak7719 = NULL;
	int rst_pin;

	ak7719 = devm_kzalloc(&i2c->dev, sizeof(struct ak7719_data), GFP_KERNEL);
	if (ak7719 == NULL) {
		printk("kzalloc for ak7719 is failed\n");
		return -ENOMEM;
	}

	rst_pin = of_get_gpio_flags(np, 0, &flags);
	if (rst_pin < 0 || !gpio_is_valid(rst_pin)) {
		printk("ak7719 rst pin is not working");
		rval = -ENXIO;
		goto out;
	}

	ak7719->rst_pin = rst_pin;
	ak7719->rst_active = !!(flags & OF_GPIO_ACTIVE_LOW);

	ak7719->client = i2c;
	i2c_set_clientdata(i2c, ak7719);


	rval = devm_gpio_request(&i2c->dev, ak7719->rst_pin, "ak7719 reset");
	if (rval < 0){
		dev_err(&i2c->dev, "Failed to request rst_pin: %d\n", rval);
		return rval;
	}

	/* Reset AK7719 dsp */
	gpio_direction_output(rst_pin, ak7719->rst_active);
	msleep(10);
	gpio_direction_output(rst_pin, !ak7719->rst_active);
	msleep(10);

	ak7719_read_reg(ak7719, 0x50);
	ak7719_read_reg(ak7719, 0x51);
	ak7719_write_reg(ak7719, 0xd0, 0x1);
	ak7719_write_reg(ak7719, 0xd1, 0x1);
	ak7719_write_reg(ak7719, 0xc0, 0x20);
	ak7719_write_reg(ak7719, 0xc1, 0x30);
	ak7719_write_reg(ak7719, 0xc2, 0x64);
	ak7719_write_reg(ak7719, 0xc3, 0xb2);
	if (aec == 0 ){
		ak7719_write_reg(ak7719, 0xc4, 0x00);
	}else{
		ak7719_write_reg(ak7719, 0xc4, 0xf0);
	}
	ak7719_write_reg(ak7719, 0xc5, 0x0);
	ak7719_write_reg(ak7719, 0xc6, 0x0);
	//ak7719_write_reg(ak7719, 0xc7, 0x0);
	ak7719_write_reg(ak7719, 0xc8, 0x80);
//	ak7719_read_reg(ak7719, 0x50);
//	ak7719_read_reg(ak7719, 0x51);
//	ak7719_read_reg(ak7719, 0x47);

	ak7719_write_reg(ak7719, 0xc6, 0x20);
	msleep(1);

	ak7719_wq = create_workqueue("ak7719_workqueue");
	ak7719_work.data = ak7719;
	INIT_WORK(&(ak7719_work.download_binary), ak7719_download_work);
	queue_work(ak7719_wq, &(ak7719_work.download_binary));

	printk("ak7719 init done with AEC mode: %s \n",aec == 1 ?"On":"Off");

	return 0;
out:
	devm_kfree(&i2c->dev, ak7719);
	return rval;
}

static int ak7719_i2c_remove(struct i2c_client *client)
{	struct ak7719_data *ak7719 = NULL;

	destroy_workqueue(ak7719_wq);
	ak7719_wq = NULL;
	ak7719_work.data = NULL;
	ak7719 = i2c_get_clientdata(client);
	devm_gpio_free(&client->dev, ak7719->rst_pin);
	devm_kfree(&client->dev, ak7719);

	return 0;
}

static const struct of_device_id ak7719_dt_ids[] = {
	{ .compatible = "ambarella,ak7719",},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ak7719_dt_ids);

static const struct i2c_device_id ak7719_i2c_id[] = {
	{ "ak7719", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ak7719_i2c_id);

static struct i2c_driver ak7719_i2c_driver = {
	.driver = {
		.name = "DRIVER_NAME",
		.owner = THIS_MODULE,
		.of_match_table = ak7719_dt_ids,
	},
	.probe		=	ak7719_i2c_probe,
	.remove		=	ak7719_i2c_remove,
	.id_table	=	ak7719_i2c_id,
};
#endif

static int __init ak7719_modinit(void)
{
	int ret;

#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)
	ret = i2c_add_driver(&ak7719_i2c_driver);
	if (ret != 0)
		pr_err("Failed to register AK7719 I2C driver: %d\n", ret);
#endif
	return ret;
}
module_init(ak7719_modinit);
static void __exit ak7719_exit(void)
{
#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)
	i2c_del_driver(&ak7719_i2c_driver);
#endif
}
module_exit(ak7719_exit);

MODULE_DESCRIPTION("Amabrella Board AK7719 DSP for Audio Codec");
MODULE_AUTHOR("Ken He <jianhe@ambarella.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:"DRIVER_NAME);
