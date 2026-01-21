#include "debug.h"
#include "ch32v20x.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "wchnet.h"
#include "UDP.h"
#include "GPIO.h"
#include "tim.h"
#include "app_logic.h"
#include "eth_driver.h"
#include "ch32v20x_it.h"

// MQTT 全局缓冲（UDP.c 的 socket 中断里引用了 extern，为了不改动太大这里保留定义）
uint8_t MqttTxBuffer[1024];
uint8_t MqttRxBuffer[2048];
volatile uint16_t MqttRxLen = 0;

extern void WCHNET_MainTask(void);

// 来自中断文件：毫秒 tick
extern volatile uint32_t g_net_tick_ms;

// ====================== 串口2 JSON 接收配置 ======================
#define UART2_JSON_BAUD        115200
#define UART2_JSON_BUF_SIZE    2048

static void UART2_JsonRx_Init(uint32_t baud)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

    // PA2: USART2_TX（可选，调试用；不接也没关系）
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    // PA3: USART2_RX（接 NanoPi TX）
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    USART_InitStructure.USART_BaudRate = baud;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(USART2, &USART_InitStructure);

    // RXNE 中断
    USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel = USART2_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    USART_Cmd(USART2, ENABLE);

    UART2_Ring_Clear();
}

// ====================== JSON 帧提取（括号深度法） ======================
static char s_json_buf[UART2_JSON_BUF_SIZE];
static uint16_t s_json_len = 0;
static int s_depth = 0;
static uint8_t s_in_string = 0;
static uint8_t s_escape = 0;

static void json_rx_reset(void)
{
    s_json_len = 0;
    s_depth = 0;
    s_in_string = 0;
    s_escape = 0;
}

static void process_uart2_json_stream(void)
{
    uint8_t b;
    while (UART2_Ring_ReadByte(&b))
    {
        char c = (char)b;

        // 等待 JSON 开始
        if (s_depth == 0)
        {
            if (c == '{')
            {
                json_rx_reset();
                s_depth = 1;
                s_json_buf[s_json_len++] = c;
            }
            else
            {
                // ignore
            }
            continue;
        }

        // 已经在收 JSON
        if (s_json_len >= (UART2_JSON_BUF_SIZE - 1))
        {
            // overflow，丢弃本帧
            json_rx_reset();
            continue;
        }

        s_json_buf[s_json_len++] = c;

        if (s_in_string)
        {
            if (s_escape)
            {
                s_escape = 0;
            }
            else if (c == '\\')
            {
                s_escape = 1;
            }
            else if (c == '"')
            {
                s_in_string = 0;
            }
        }
        else
        {
            if (c == '"')
            {
                s_in_string = 1;
            }
            else if (c == '{')
            {
                s_depth++;
            }
            else if (c == '}')
            {
                s_depth--;
                if (s_depth <= 0)
                {
                    // 一帧结束
                    s_json_buf[s_json_len] = '\0';

                    printf("UART2 JSON RX: %s\r\n", s_json_buf);
                    (void)AppScheduler_HandleJson(s_json_buf);

                    json_rx_reset();
                }
            }
        }
    }
}

int main(void)
{
    SystemInit();

    // printf 输出串口（默认 DEBUG_UART2；如你希望调试串口用 USART1，请在 debug.h 里改 DEBUG）
    USART_Printf_Init(115200);

    MCU_GPIO_Init();
    Delay_Init();

    Delay_Ms(500);

    // TIM1/TIM2
    TIM1_Int_Init(120 - 1, 1000 - 1);
    TIM2_Init();

    // 串口2：接收 NanoPi JSON
    UART2_JsonRx_Init(UART2_JSON_BAUD);

    // 调度器
    AppScheduler_Init();

    printf("Init WCHNET...\r\n");
    printf("Local IP: %d.%d.%d.%d\r\n", IPAddr[0], IPAddr[1], IPAddr[2], IPAddr[3]);
    printf("GW IP   : %d.%d.%d.%d\r\n", GWIPAddr[0], GWIPAddr[1], GWIPAddr[2], GWIPAddr[3]);

    if (ds1307_pro() == 0) printf("DS1307 ACK OK\r\n");
    else printf("DS1307 ACK FAIL\r\n");


    // 网卡初始化
    WCHNET_GetMacAddr(MACAddr);
    uint8_t i = ETH_LibInit(IPAddr, GWIPAddr, IPMask, MACAddr);
    mStopIfError(i);

    // UDP socket
    WCHNET_CreateUdpSocket();

    while (1)
    {
        // WCHNET 驱动任务
        WCHNET_MainTask();
        if (WCHNET_QueryGlobalInt()) {
            WCHNET_HandleGlobalInt();
        }

        uint32_t now = g_net_tick_ms;

        // 串口2：处理 JSON 帧
        process_uart2_json_stream();

        // 调度器 tick（内部每秒读一次 RTC 并触发 on/off）
        AppScheduler_Tick(now);

        // ===== Debug: 每 15 秒打印一次外部 RTC 时间 =====
        static uint32_t s_last_rtc_print_ms = 0;
        if ((uint32_t)(now - s_last_rtc_print_ms) >= 15000U)
        {
            s_last_rtc_print_ms = now;
            char ts[24];
            if (AppRTC_GetNowString(ts, sizeof(ts)) == 0)
            {
                printf("[RTC] %s\r\n", ts);
            }
            else
            {
                printf("[RTC] read error\r\n");
            }
        }
    }
}
