#include "eeprom.h"
#include "main.h"

extern I2C_HandleTypeDef hi2c1;   /* I2C1 句柄，定义在 main.c */

/* 写一个字节：addr 格子编号 0~255，data 要写的数据；返回 HAL 状态（0=成功） */
uint8_t AT24C02_Write_Byte(uint8_t addr, uint8_t data)
{
    uint8_t ret;
    ret = HAL_I2C_Mem_Write(&hi2c1, AT24C02_ADDR, addr, I2C_MEMADD_SIZE_8BIT, &data, 1, 100);
    HAL_Delay(5);   /* 等待写周期完成（最长 5ms） */
    return ret;
}

/* 读一个字节：addr 格子编号 0~255；返回读到的数据 */
uint8_t AT24C02_Read_Byte(uint8_t addr)
{
    uint8_t data;
    HAL_I2C_Mem_Read(&hi2c1, AT24C02_ADDR, addr, I2C_MEMADD_SIZE_8BIT, &data, 1, 100);
    return data;
}
