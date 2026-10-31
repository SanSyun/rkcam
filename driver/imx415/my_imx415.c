#include "my_imx415.h"

//==================变量=================//
static const char *const imx415_supply_names[] = {
    "dvdd", //数字核心电源
    "dovdd", //数字IO电源
    "avdd", //模拟电源
};

//读外设寄存器
static int imx415_read_reg(imx415 *dev, uint16_t addr, void *val, int len){
    int ret;
    struct i2c_msg msg[2]; //因为读iic的时候会先写设备地址然后才是读数据，所以这个里需要构造两个msg
    struct i2c_client *client = (struct i2c_client *)dev->client;

    if(!val || len <=0 || len >4){
        return -EINVAL;
    }

    uint8_t reg_buf[2];
    reg_buf[0] = (addr >> 8) & 0xff;
    reg_buf[1] = addr & 0xff;

    //往哪里读
    msg[0].addr = client->addr;
    msg[0].flags = 0; //写
    msg[0].buf = reg_buf; //读的寄存器
    msg[0].len = sizeof(reg_buf)/sizeof(uint8_t);

    //读什么东西
    msg[1].addr = client->addr;
    msg[1].flags = I2C_M_RD; //读
    msg[1].buf = val; //读的寄存器数据
    msg[1].len = len; //读的寄存器数据长度

    ret = i2c_transfer(client->adapter, msg, 2);
    if(ret == 2){
        ret = 0;
    }
    else{
        printk("i2c read data failed=%d from reg = %x len = %d\n", ret, addr, len);
        ret = -EREMOTEIO;
    }

    return ret;
}

//写外设寄存器
static int imx415_write_reg(imx415 *dev, uint16_t addr, uint32_t val, int len){
    int ret;
    int val_i = 0;
    struct i2c_msg msg;
    struct i2c_client *client = (struct i2c_client *)dev->client;
    uint8_t buf[6]; //最大支持16bit addr + 32bit value

    if(len <= 0 || len > 4){
        return -EINVAL;
    }

    //写地址
    buf[0] = (uint8_t)(addr>>8)&0xff;
    buf[1] = (uint8_t)(addr&0xff);

    //写值
    for(val_i = 0; val_i < len; val_i++){
        buf[2+val_i] = (uint8_t)(val>>(8*val_i)&0xff);
    }

    msg.addr = client->addr;
    msg.flags = 0; //写
    msg.buf = buf;
    msg.len = len + 2;

    ret = i2c_transfer(client->adapter, &msg, 1);
    if(ret==1){
        return 0;
    }
    else{
        printk("i2c write data failed=%d from reg = %x len = %d\n", ret, addr, len);
        ret = -EREMOTEIO;
    }
    return ret;
}

//配置regulators
static int imx415_configure_regulators(struct imx415 *imx415){
    unsigned int i;
    for(i = 0; i < IMX415_NUM_SUPPLIES; i++){
        imx415->supplies[i].supply = imx415_supply_names[i];
    }
    return devm_regulator_bulk_get(&imx415->client->dev, IMX415_NUM_SUPPLIES, imx415->supplies);
}

//Power ON && Power Off
static int __imx415_power_on(struct imx415 *imx415){
    int ret;
    struct device *dev = &imx415->client->dev;

    //pinctrl default
    if(!IS_ERR_OR_NULL(imx415->pins_default)){
        ret = pinctrl_select_state(imx415->pinctrl, imx415->pins_default);
        if(ret < 0){
            dev_err(dev, "could not set pins\n");
        }    
    }

    //regulator enable
    ret = regulator_bulk_enable(IMX415_NUM_SUPPLIES, imx415->supplies);
    if(ret < 0){
        dev_err(dev, "failed to enable regulators\n");
        goto err_pinctrl;
    }

    //power GPIO = 1
    if(!IS_ERR(imx415->power_gpio)){
        gpiod_direction_output(imx415->power_gpio, 1);    
    }
    usleep_range(10*1000, 20*1000);

    //reset GPIO = 0
    if(!IS_ERR(imx415->reset_gpio)){
        gpiod_direction_output(imx415->reset_gpio, 0);
    }
    usleep_range(10*1000, 20*1000);

    //xvclk = 37.125MHz
    ret = clk_set_rate(imx415->xvclk, IMX415_XVCLK_FREQ);
    if(ret < 0){
        dev_warn(dev, "failed to set xvclk rate\n");
    }
    if(clk_get_rate(imx415->xvclk) != IMX415_XVCLK_FREQ){
        dev_warn(dev, "failed to match xvclk\n");
    }

    //enable xvclk
    ret = clk_prepare_enable(imx415->xvclk);
    if(ret < 0){
        dev_err(dev, "failed to enable xvclk\n");
        goto err_clk;
    }

    usleep_range(20*1000, 30*1000);

    return 0;

err_pinctrl:
    if(!IS_ERR_OR_NULL(imx415->pins_sleep)){
        pinctrl_select_state(imx415->pinctrl, imx415->pins_sleep);
    }
    return ret;

err_clk:
    if(!IS_ERR(imx415->reset_gpio)){
        gpiod_direction_output(imx415->reset_gpio, 1);
    }
    regulator_bulk_disable(IMX415_NUM_SUPPLIES, imx415->supplies);
    return ret;
}

// static int __imx415_power_off(struct imx415 *imx415){
//     int ret;
//     struct device *dev = &imx415->client->dev;

//     if(!IS_ERR(imx415->reset_gpio)){
//         gpiod_direction_output(imx415->reset_gpio, 1);
//     }

//     clk_disable_unprepare(imx415->xvclk);

//     if(!IS_ERR_OR_NULL(imx415->pins_sleep)){
//         ret = pinctrl_select_state(imx415->pinctrl, imx415->pins_sleep);
//         if(ret < 0){
//             dev_warn(dev, "could not set pins\n");
//         }
//     }

//     if(!IS_ERR(imx415->power_gpio)){
//         gpiod_direction_output(imx415->power_gpio, 0);
//     }

//     regulator_bulk_disable(IMX415_NUM_SUPPLIES, imx415->supplies);

//     return 0;
// }

static int imx415_check_sensor_id(struct imx415 *imx415, struct i2c_client *client){
    int ret;
    struct device *dev = &imx415->client->dev;
    uint8_t id = 0;

    ret = imx415_read_reg(imx415, IMX415_REG_CHIP_ID, &id, 1);
    if(id != IMX415_CHIP_ID){
        dev_err(dev, "unexpected sensor id(%06x), ret(%d)\n", id, ret);
        return -ENODEV;
    }
    dev_info(dev, "detected imx415 id %06x\n", id);
    return 0;
}

static int imx415_probe(struct i2c_client *client, const struct i2c_device_id *id){
    int ret;
   
    struct device *dev = &client->dev; 
    // struct device_node *node = dev->of_node; //返回匹配后的设备树节点
    struct imx415 *imx415;

    printk(KERN_ERR "========== sc_imx415 probe entered ==========\n"); 

    imx415 = devm_kzalloc(dev, sizeof(*imx415), GFP_KERNEL);
    if(!imx415){
        return -ENOMEM;
    }

    //iic
    imx415->client = client;

    //clk
    imx415->xvclk = devm_clk_get(dev, "xvclk");
    if(IS_ERR(imx415->xvclk)){
        dev_err(dev, "failed to get xvclk\n");
        return -EINVAL;
    }

    //gpio
    imx415->reset_gpio = devm_gpiod_get(dev, "reset", 0);
    if(IS_ERR(imx415->reset_gpio)){
        dev_warn(dev, "failed to get reset-gpios\n");
    }
    imx415->power_gpio = devm_gpiod_get(dev, "power", 0);
    if(IS_ERR(imx415->power_gpio)){
        dev_warn(dev, "failed to get power-gpios\n");
    }

    //pinctrl
    imx415->pinctrl = devm_pinctrl_get(dev);
    if(!IS_ERR(imx415->pinctrl)){
        imx415->pins_default = pinctrl_lookup_state(imx415->pinctrl, "rockchip,camera_default");
        if(IS_ERR(imx415->pins_default)){
            dev_info(dev, "could not get default pinstate\n");
        }
    }
    else{
        dev_info(dev, "no pinctrl\n");
    }

    //regulators
    ret = imx415_configure_regulators(imx415);
    if(ret){
        dev_err(dev, "failed to get regulator\n");
        return ret;
    }

    ret = __imx415_power_on(imx415);
    if(ret){
        dev_err(dev, "failed to power on\n");
        return ret;
    }

    //检查sensor id
    ret = imx415_check_sensor_id(imx415, client);
    if(ret){
        dev_err(dev, "failed to check imx415 sensor id\n");
    }

    return 0;
}

static int imx415_remove(struct i2c_client *client){
    
    return 0;
}

static const struct i2c_device_id imx415_id[] = {
    {"sony,imx415", 0},
    {}
};

static const struct of_device_id imx415_of_match[] = {
    { .compatible = "sony,imx415" },
    { }
};

static struct i2c_driver imx415_driver = {
    .probe = imx415_probe,
    .remove = imx415_remove,
    .driver = {
        .owner = THIS_MODULE,
        .name = imx415_NAME,
        .of_match_table = imx415_of_match,
    },
    .id_table = imx415_id,
};

//驱动入口函数
static int __init imx415_init(void){
    int ret = 0;

    ret = i2c_add_driver(&imx415_driver);
    return ret;
}

//驱动出口函数
static void __exit imx415_exit(void){
    i2c_del_driver(&imx415_driver);
}


module_init(imx415_init);
module_exit(imx415_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("sc");
MODULE_INFO(intree, "Y");