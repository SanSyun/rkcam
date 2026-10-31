#ifndef AP3215C_H
#define AP3215C_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/ide.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/of_gpio.h>
#include <linux/semaphore.h>
#include <linux/timer.h>
#include <linux/i2c.h>
#include <asm/uaccess.h>
#include <asm/io.h>

#include <linux/regulator/consumer.h>
#include <linux/clk.h>
#include <linux/gpio.h>


//imx415设备控制寄存器宏定义
#define imx415_NAME            "imx415"
#define IMX415_XVCLK_FREQ       37125000
#define IMX415_NUM_SUPPLIES     3

#define IMX415_REG_CHIP_ID      0x311A
#define IMX415_CHIP_ID          0xE0

typedef struct imx415{
    //i2c
    struct i2c_client *client;

    //时钟
    struct clk *xvclk;

    //GPIO
    struct gpio_desc *reset_gpio;
    struct gpio_desc *power_gpio;

    //供电
    struct regulator_bulk_data supplies[IMX415_NUM_SUPPLIES];

    struct pinctrl *pinctrl;
    struct pinctrl_state *pins_default;
    struct pinctrl_state *pins_sleep;
}imx415;



#endif //AP3215C_H