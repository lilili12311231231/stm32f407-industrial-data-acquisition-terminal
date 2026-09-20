#ifndef __EEPROM_H__
#define __EEPROM_H__

#include <stdint.h>

/* AT24C02 设备地址：7 位门牌 0x50 左移一位 = 0xA0，读写方向位由 HAL 内部补 */
#define AT24C02_ADDR 0xA0

uint8_t AT24C02_Write_Byte(uint8_t addr, uint8_t data);  /* 写一个字节到指定格子 */
uint8_t AT24C02_Read_Byte(uint8_t addr);                 /* 从指定格子读一个字节 */

#endif
