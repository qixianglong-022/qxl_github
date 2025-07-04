#ifndef INC_DEBUG_LOG_H_
#define INC_DEBUG_LOG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdarg.h>

// --- �û������� ---
#define LOG_BUFFER_SIZE 128 // ��־�����������������Ը�����Ҫ����

// --- ��־��¼���� ---

// ���жϻ�ʱ�����еĴ����е����������
// ���ǳ��죬ֻ�ǽ�һ���ַ���ָ����뻺����
void Log_Debug(const char* message);
void Log_Debug1(const char *format, ...);


// --- ϵͳ���ɺ��� ---

// �� main ������ʼʱ����һ��
void Log_Init(void);


// �� main ������ while(1) ѭ���г�������
void Log_Process(void);


#ifdef __cplusplus
}
#endif

#endif /* INC_DEBUG_LOG_H_ */
