#include "debug_log.h"
#include <stdio.h>
#include <string.h>
#include "main.h"
#include <stdarg.h> // ������ͷ�ļ�

// ���λ������ṹ
static volatile struct {
    const char* buffer[LOG_BUFFER_SIZE];
    volatile int head;
    volatile int tail;
} log_queue;

// ��ʼ����־ϵͳ
void Log_Init(void) {
    log_queue.head = 0;
    log_queue.tail = 0;
}

// ���жϰ�ȫ����һ����־��Ϣ�������
void Log_Debug(const char* message) {
    int next_head = (log_queue.head + 1) % LOG_BUFFER_SIZE;
    // ������������ˣ��Ͷ�����ɵ���Ϣ������ʲô��������
    if (next_head != log_queue.tail) {
        log_queue.buffer[log_queue.head] = message;
        log_queue.head = next_head;
    }
}

// ����ѭ���д�����ӡ��־
void Log_Process(void) {
    // ��黺�������Ƿ�������
    if (log_queue.tail != log_queue.head) {
        // ��β��ȡ����Ϣ
        const char* message = log_queue.buffer[log_queue.tail];
        // ����β��ָ��
        log_queue.tail = (log_queue.tail + 1) % LOG_BUFFER_SIZE;
        
        // �����ﰲȫ�ص��� printf
        printf("%s\r\n", message);
    }
}

void Log_Debug1(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    
    // ʹ�� vsnprintf ��ȫ�ظ�ʽ���ַ���
    char formatted_msg[128];
    vsnprintf(formatted_msg, sizeof(formatted_msg), format, args);
    va_end(args);
    
    // Ȼ�󽫸�ʽ������ַ�����ӵ���־����
    //Log_Enqueue(LOG_LEVEL_DEBUG, formatted_msg);
}
