#include "ch32v20x_it.h"
#include "eth_driver.h"
#include "wchnet.h"

// 毫秒 tick（给 main.c 用）
volatile uint32_t g_net_tick_ms = 0;

// ================= USART2 RX Ring Buffer =================
static volatile uint8_t  s_uart2_rx_buf[UART2_RX_BUF_SIZE];
static volatile uint16_t s_uart2_rx_head = 0;
static volatile uint16_t s_uart2_rx_tail = 0;

static inline uint16_t rb_next(uint16_t v)
{
    v++;
    if (v >= UART2_RX_BUF_SIZE) v = 0;
    return v;
}

int UART2_Ring_ReadByte(uint8_t *out)
{
    if (!out) return 0;
    if (s_uart2_rx_head == s_uart2_rx_tail) return 0;
    *out = s_uart2_rx_buf[s_uart2_rx_tail];
    s_uart2_rx_tail = rb_next(s_uart2_rx_tail);
    return 1;
}

uint16_t UART2_Ring_Available(void)
{
    uint16_t head = s_uart2_rx_head;
    uint16_t tail = s_uart2_rx_tail;
    if (head >= tail) return (uint16_t)(head - tail);
    return (uint16_t)(UART2_RX_BUF_SIZE - (tail - head));
}

void UART2_Ring_Clear(void)
{
    s_uart2_rx_head = 0;
    s_uart2_rx_tail = 0;
}

// ===================== Interrupt Handlers =====================
void NMI_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void HardFault_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void ETH_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void TIM2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void TIM1_UP_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USART2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

void NMI_Handler(void) {}

void HardFault_Handler(void)
{
    printf("HardFault_Handler\r\n");
    while (1) {}
}

void ETH_IRQHandler(void)
{
    WCHNET_ETHIsr();
}

void TIM2_IRQHandler(void)
{
    // WCHNET 定时器 ISR（周期 WCHNETTIMERPERIOD ms）
    WCHNET_TimeIsr(WCHNETTIMERPERIOD);
    g_net_tick_ms += WCHNETTIMERPERIOD;

    TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
}

void TIM1_UP_IRQHandler(void)
{
    TIM_ClearITPendingBit(TIM1, TIM_IT_Update);
}

void USART2_IRQHandler(void)
{
    if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET)
    {
        uint8_t ch = (uint8_t)USART_ReceiveData(USART2);

        uint16_t next = rb_next(s_uart2_rx_head);
        if (next != s_uart2_rx_tail) {
            s_uart2_rx_buf[s_uart2_rx_head] = ch;
            s_uart2_rx_head = next;
        } else {
            // overflow: drop byte
        }

        USART_ClearITPendingBit(USART2, USART_IT_RXNE);
    }
}
