/********************************** (C) COPYRIGHT *******************************
* File Name          : ch32v20x_it.h
* Description        : Interrupt handlers + small ISR utilities.
*******************************************************************************/
#ifndef __CH32V20X_IT_H
#define __CH32V20X_IT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "debug.h"
#include <stdint.h>

// 毫秒 tick（在 TIM2 中断里累加）
extern volatile uint32_t g_net_tick_ms;

// ================= USART2 RX Ring Buffer =================
// NanoPi 通过串口2下发 JSON 时，可从 ring buffer 取数据
#ifndef UART2_RX_BUF_SIZE
#define UART2_RX_BUF_SIZE  2048
#endif

/**
 * @brief  从 USART2 RX ring buffer 读取 1 字节
 * @param  out - 输出字节
 * @return 1=读到了，0=没有数据
 */
int UART2_Ring_ReadByte(uint8_t *out);

/**
 * @brief  ring buffer 可读字节数（近似）
 */
uint16_t UART2_Ring_Available(void);

/**
 * @brief  清空 ring buffer
 */
void UART2_Ring_Clear(void);

#ifdef __cplusplus
}
#endif

#endif /* __CH32V20X_IT_H */
