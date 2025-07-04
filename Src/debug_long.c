#include "debug_log.h"
#include <stdio.h>
#include <string.h>
#include "main.h"
#include <stdarg.h> // 添加这个头文件

// 环形缓冲区结构
static volatile struct {
    const char* buffer[LOG_BUFFER_SIZE];
    volatile int head;
    volatile int tail;
} log_queue;

// 初始化日志系统
void Log_Init(void) {
    log_queue.head = 0;
    log_queue.tail = 0;
}

// （中断安全）将一条日志消息放入队列
void Log_Debug(const char* message) {
    int next_head = (log_queue.head + 1) % LOG_BUFFER_SIZE;
    // 如果缓冲区满了，就丢弃最旧的消息（或者什么都不做）
    if (next_head != log_queue.tail) {
        log_queue.buffer[log_queue.head] = message;
        log_queue.head = next_head;
    }
}

// 在主循环中处理并打印日志
void Log_Process(void) {
    // 检查缓冲区中是否有数据
    if (log_queue.tail != log_queue.head) {
        // 从尾部取出消息
        const char* message = log_queue.buffer[log_queue.tail];
        // 更新尾部指针
        log_queue.tail = (log_queue.tail + 1) % LOG_BUFFER_SIZE;
        
        // 在这里安全地调用 printf
        printf("%s\r\n", message);
    }
}

void Log_Debug1(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    
    // 使用 vsnprintf 安全地格式化字符串
    char formatted_msg[128];
    vsnprintf(formatted_msg, sizeof(formatted_msg), format, args);
    va_end(args);
    
    // 然后将格式化后的字符串添加到日志队列
    //Log_Enqueue(LOG_LEVEL_DEBUG, formatted_msg);
}
