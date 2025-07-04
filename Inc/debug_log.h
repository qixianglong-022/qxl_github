#ifndef INC_DEBUG_LOG_H_
#define INC_DEBUG_LOG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdarg.h>

// --- 用户可配置 ---
#define LOG_BUFFER_SIZE 128 // 日志缓冲区的容量，可以根据需要调整

// --- 日志记录函数 ---

// 在中断或时间敏感的代码中调用这个函数
// 它非常快，只是将一个字符串指针放入缓冲区
void Log_Debug(const char* message);
void Log_Debug1(const char *format, ...);


// --- 系统集成函数 ---

// 在 main 函数开始时调用一次
void Log_Init(void);


// 在 main 函数的 while(1) 循环中持续调用
void Log_Process(void);


#ifdef __cplusplus
}
#endif

#endif /* INC_DEBUG_LOG_H_ */
