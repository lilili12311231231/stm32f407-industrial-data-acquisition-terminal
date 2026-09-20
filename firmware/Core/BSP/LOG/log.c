#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "cmsis_os.h"
#include "../../Inc/main.h"
#include "../../../Middlewares/FATFS/ff.h"

#define LOG_FILE_MAX_SIZE (1024*1024)  /* 日志文件大小阈值(字节)，超过则轮转 */
#define LOG_MSG_LEN       128          /* 单条日志最大长度(字节) */
#define LOG_QUEUE_LEN     16           /* 日志队列深度，满则丢弃新日志 */

typedef struct
{
    char text[LOG_MSG_LEN];            /* 一条格式化好的日志字符串 */
} log_msg_t;

static log_level_t s_level = LOG_LEVEL_DEBUG;   /* 日志等级阈值，低于此等级不打印 */

static FATFS s_fs;              /* FATFS 对象，挂载后需常驻内存 */
static FIL   s_log_file;        /* log.txt 文件句柄 */
static int   s_log_ready = 0;   /* 1 = SD 卡已挂载且 log.txt 已打开 */
static FSIZE_t s_log_size = 0;  /* 当前 log.txt 累计大小 */

static osMessageQueueId_t s_log_queue = NULL;   /* 日志消息队列句柄 */

void log_set_level(log_level_t level)
{
    s_level = level; /* 设置日志等级阈值 */
}

/* 创建日志队列，调度器启动前调用一次 */
void log_init(void)
{
    s_log_queue = osMessageQueueNew(LOG_QUEUE_LEN, sizeof(log_msg_t), NULL);
}

/* 挂载 SD 卡并打开 log.txt，开机时调用一次。返回 0 成功，非 0 失败 */
int log_fs_init(void)
{
    FRESULT fres;

    fres = f_mount(&s_fs, "0:", 1);
    if (fres != FR_OK) return 1;

    fres = f_open(&s_log_file, "0:log.txt", FA_OPEN_APPEND | FA_WRITE);
    if (fres != FR_OK) return 2;

    s_log_size = f_size(&s_log_file);  /* 追加模式打开时，记录已有文件大小 */
    s_log_ready = 1;
    return 0;
}

/* 格式化一条日志并入队。调用方只做这一步，真正的 printf/写卡由 LogTask 完成 */
void log_output(log_level_t level, const char *file, int line, const char *fmt, ...)
{
    if (level < s_level) return; /* 日志等级低于阈值则不打 */

    log_msg_t msg;
    int head;
    const char *tag = "";

    switch (level)
    {
        case LOG_LEVEL_DEBUG: tag = "[DBG] "; break;
        case LOG_LEVEL_INFO:  tag = "[INF] "; break;
        case LOG_LEVEL_WARN:  tag = "[WAR] "; break;
        case LOG_LEVEL_ERROR: tag = "[ERR] "; break;
    }

    /* 整条日志先格式化成一行，串口和 SD 卡写同一份内容 */
    head = snprintf(msg.text, sizeof(msg.text), "%s[%u] %s:%d ",
                    tag, (unsigned)HAL_GetTick(), file, line);
    if (head < 0) head = 0;
    if (head >= (int)sizeof(msg.text)) head = (int)sizeof(msg.text) - 1;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg.text + head, sizeof(msg.text) - head, fmt, ap);
    va_end(ap);

    if (s_log_queue == NULL)
    {
        printf("%s", msg.text);   /* 队列还没建好，退化同步串口输出 */
        return;
    }

    /* 非阻塞入队，队列满则丢弃这条日志（日志允许丢，不能阻塞调用方） */
    osMessageQueuePut(s_log_queue, &msg, 0U, 0U);
}

/* 日志后台任务：从队列取日志，串口打印 + 写 SD 卡（含轮转） */
void LogTask(void *argument)
{
    log_msg_t msg;

    for (;;)
    {
        if (osMessageQueueGet(s_log_queue, &msg, NULL, osWaitForever) != osOK)
        {
            continue;
        }

        printf("%s", msg.text);   /* 串口 */

        if (s_log_ready)          /* 落盘到 SD 卡 */
        {
            UINT bw;
            f_write(&s_log_file, msg.text, strlen(msg.text), &bw);
            s_log_size += bw;
            f_sync(&s_log_file);

            /* 超过阈值则轮转：log.txt 改名 log_old.txt，新建空 log.txt */
            if (s_log_size >= LOG_FILE_MAX_SIZE)
            {
                f_close(&s_log_file);
                f_unlink("0:log_old.txt");               /* 删掉上一个 old */
                f_rename("0:log.txt", "0:log_old.txt");  /* 当前改名存档 */
                f_open(&s_log_file, "0:log.txt", FA_CREATE_ALWAYS | FA_WRITE);
                s_log_size = 0;
            }
        }
    }
}
