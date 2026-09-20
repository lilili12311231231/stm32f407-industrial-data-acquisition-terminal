/**
 * diskio.c - FATFS low-level disk I/O layer
 *
 * Adapted from ALIENTEK example. Kept only the SD card (SDIO) backend,
 * removed the NOR FLASH branch and the ALIENTEK-only includes.
 */

#include "diskio.h"
#include "../../Core/BSP/SDIO/sdio_sdcard.h"

#define SD_CARD  0   /* SD card = physical drive number 0 */


DSTATUS disk_status (
    BYTE pdrv       /* Physical drive number */
)
{
    return RES_OK;
}


DSTATUS disk_initialize (
    BYTE pdrv       /* Physical drive number */
)
{
    uint8_t res = 0;

    if (pdrv == SD_CARD)
    {
        res = sd_init();    /* SD card init */
    }
    else
    {
        res = 1;
    }

    return res ? STA_NOINIT : 0;
}


DRESULT disk_read (
    BYTE pdrv,      /* Physical drive number */
    BYTE *buff,     /* Data buffer to store read data */
    LBA_t sector,   /* Sector address in LBA */
    UINT count      /* Number of sectors to read */
)
{
    uint8_t res = 0;

    if (!count) return RES_PARERR;

    if (pdrv == SD_CARD)
    {
        res = sd_read_disk(buff, sector, count);

        while (res)     /* read error, re-init and retry */
        {
            sd_init();
            res = sd_read_disk(buff, sector, count);
        }
    }
    else
    {
        res = 1;
    }

    return res ? RES_ERROR : RES_OK;
}


DRESULT disk_write (
    BYTE pdrv,          /* Physical drive number */
    const BYTE *buff,   /* Data to be written */
    LBA_t sector,       /* Sector address in LBA */
    UINT count          /* Number of sectors to write */
)
{
    uint8_t res = 0;

    if (!count) return RES_PARERR;

    if (pdrv == SD_CARD)
    {
        res = sd_write_disk((uint8_t *)buff, sector, count);

        while (res)     /* write error, re-init and retry */
        {
            sd_init();
            res = sd_write_disk((uint8_t *)buff, sector, count);
        }
    }
    else
    {
        res = 1;
    }

    return res ? RES_ERROR : RES_OK;
}


DRESULT disk_ioctl (
    BYTE pdrv,      /* Physical drive number */
    BYTE cmd,       /* Control code */
    void *buff      /* Buffer to send/receive control data */
)
{
    DRESULT res;

    if (pdrv == SD_CARD)
    {
        switch (cmd)
        {
            case CTRL_SYNC:
                res = RES_OK;
                break;

            case GET_SECTOR_SIZE:
                *(DWORD *)buff = 512;
                res = RES_OK;
                break;

            case GET_BLOCK_SIZE:
                *(WORD *)buff = g_sd_card_info_handle.LogBlockSize;
                res = RES_OK;
                break;

            case GET_SECTOR_COUNT:
                *(DWORD *)buff = g_sd_card_info_handle.LogBlockNbr;
                res = RES_OK;
                break;

            default:
                res = RES_PARERR;
                break;
        }
    }
    else
    {
        res = RES_ERROR;
    }

    return res;
}
