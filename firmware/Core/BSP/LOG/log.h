#ifndef __LOG_H__
#define __LOG_H__
#define LOG_ENABLE 1 /** 日志总开关 */

/**
 * @brief 日志等级枚举
 */
typedef enum
{
    LOG_LEVEL_ERROR = 3,/** 错误日志 */
    LOG_LEVEL_WARN = 2,/** 警告日志 */
    LOG_LEVEL_INFO = 1,/** 信息日志 */
    LOG_LEVEL_DEBUG = 0,/** 调试日志 */
} log_level_t;/** 日志等级枚举 */

void log_set_level(log_level_t level); /** 设置日志等级阈值 */
void log_output(log_level_t level, const char *file, int line, const char *fmt, ...); /** 输出日志，含文件名和行号 */
int log_fs_init(void);
void log_init(void);            /** 创建日志队列，调度器启动前调用 */
void LogTask(void *argument);   /** 日志后台任务：从队列取日志落盘 */ /** 初始化日志文件系统：挂载 SD 卡 + 打开 log.txt */

/* 四个宏，分别对应不同日志等级 */
#if LOG_ENABLE
#define LOG_ERROR(fmt, ...) log_output(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) log_output(LOG_LEVEL_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) log_output(LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) log_output(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#else
#define LOG_ERROR(fmt, ...)
#define LOG_WARN(fmt, ...)
#define LOG_INFO(fmt, ...)
#define LOG_DEBUG(fmt, ...)
#endif
#endif
