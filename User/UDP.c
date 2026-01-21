#include "UDP.h"
#include "stdio.h"
#include "string.h"
#include "wchnet.h"
#include "debug.h"

#define UDP_RECE_BUF_LEN  1472

/* Global Variables */
uint8_t MACAddr[6];

// ====================== 以你的网络为准（可直接改这里） ======================
// CH32 本机 IP：10.1.5.1
uint8_t IPAddr[4]   = { 10, 1, 5, 1 };

// 默认网关：必须填写“路由器在 10.1.5.0/24 网段上的接口 IP”
// 如果你的路由器网关不是 10.1.5.254，请按现场修改。
uint8_t GWIPAddr[4] = { 10, 1, 5, 254 };

// 掩码建议 /24，避免跨网段判断异常
uint8_t IPMask[4]   = { 255, 255, 255, 0 };
// ===========================================================================

// MQTT (历史遗留，当前串口接收 JSON 用不到；保留以免工程引用报错)
uint8_t MQTT_BROKER_IP[4] = { 10, 1, 0, 141 };
uint16_t MQTT_BROKER_PORT = 1882;

uint8_t SocketId_UDP  = 0xFF;
uint8_t SocketId_MQTT = 0xFF;
uint8_t MQTT_Conn_Flag = 0;

// WCHNET socket recv buffer
uint8_t SocketRecvBuf[WCHNET_MAX_SOCKET_NUM][UDP_RECE_BUF_LEN];

// MQTT 接收缓冲（在 main.c 里定义；即使不用也保留）
extern uint8_t MqttRxBuffer[2048];
extern volatile uint16_t MqttRxLen;

void mStopIfError(uint8_t iError)
{
    if (iError == WCHNET_ERR_SUCCESS) return;
    printf("Error: %02X\r\n", (uint16_t)iError);
}

// UDP Socket (用于发送)
void WCHNET_CreateUdpSocket(void)
{
    uint8_t i;
    SOCK_INF TmpSocketInf;
    memset((void *)&TmpSocketInf, 0, sizeof(SOCK_INF));

    // 这里的 DesPort/SourPort 是 socket 默认参数。
    // 真正发送时我们使用 WCHNET_SocketUdpSendTo() 指定目的 IP/端口。
    TmpSocketInf.DesPort = UDP_DEFAULT_DPORT;
    TmpSocketInf.SourPort = 2000;
    TmpSocketInf.ProtoType = PROTO_TYPE_UDP;
    TmpSocketInf.RecvStartPoint = (uint32_t)SocketRecvBuf[0];
    TmpSocketInf.RecvBufLen = UDP_RECE_BUF_LEN;

    i = WCHNET_SocketCreat(&SocketId_UDP, &TmpSocketInf);
    printf("UDP Socket Created: %d\r\n", SocketId_UDP);
    mStopIfError(i);
}

// MQTT TCP Socket (保留)
void WCHNET_CreateMqttSocket(void)
{
    uint8_t i;
    SOCK_INF TmpSocketInf;
    memset((void *)&TmpSocketInf, 0, sizeof(SOCK_INF));

    memcpy((void *)TmpSocketInf.IPAddr, MQTT_BROKER_IP, 4);
    TmpSocketInf.DesPort = MQTT_BROKER_PORT;
    TmpSocketInf.SourPort = 3000;
    TmpSocketInf.ProtoType = PROTO_TYPE_TCP;
    TmpSocketInf.RecvStartPoint = (uint32_t)SocketRecvBuf[1];
    TmpSocketInf.RecvBufLen = UDP_RECE_BUF_LEN;

    i = WCHNET_SocketCreat(&SocketId_MQTT, &TmpSocketInf);
    printf("MQTT TCP Socket Created: %d\r\n", SocketId_MQTT);
    mStopIfError(i);

    i = WCHNET_SocketConnect(SocketId_MQTT);
    mStopIfError(i);
}

// 发送UDP数据到指定 IP/端口
void UDP_SendTo_IP(const uint8_t ip[4], uint16_t port, const char *data, uint32_t len)
{
    uint32_t send_len = len;

    if (SocketId_UDP == 0xFF || ip == NULL || data == NULL || len == 0) return;

    WCHNET_SocketUdpSendTo(SocketId_UDP, (uint8_t *)data, &send_len, (uint8_t *)ip, port);

    printf("UDP SendTo %u.%u.%u.%u:%u Len:%lu\r\n",
           ip[0], ip[1], ip[2], ip[3], port, (unsigned long)send_len);
}

// 发送UDP数据到主板: 10.1.3.{mb_id}
void UDP_SendTo_DynamicIP(uint8_t mb_id, const char *data, uint32_t len)
{
    uint8_t sip[4] = {10, 1, 3, mb_id};
    UDP_SendTo_IP(sip, UDP_DEFAULT_DPORT, data, len);
}

// 发送UDP数据到节点: 10.1.4.{node_id}
void UDP_SendTo_NodeIP(uint8_t node_id, const char *data, uint32_t len)
{
    uint8_t sip[4] = {10, 1, 4, node_id};
    UDP_SendTo_IP(sip, UDP_DEFAULT_DPORT, data, len);
}

// 群发/单点封装（主板网段）
void UDP_SendByTarget(uint8_t broadcast, uint8_t mb_id, const char *data, uint32_t len)
{
    if (!data || len == 0) return;

    if (broadcast)
    {
        // 可靠做法：逐个发（不依赖网络广播地址、掩码差异）
        for (uint8_t id = 1; id <= UDP_BROADCAST_MAX_MB_ID; id++) {
            UDP_SendTo_DynamicIP(id, data, len);
        }
    }
    else
    {
        UDP_SendTo_DynamicIP(mb_id, data, len);
    }
}

// socket 中断处理（保留：UDP 收包丢弃；MQTT TCP 收包搬运）
void WCHNET_HandleSockInt(uint8_t socketid, uint8_t intstat)
{
    if (intstat & SINT_STAT_RECV)
    {
        uint32_t len = WCHNET_SocketRecvLen(socketid, NULL);

        if (socketid == SocketId_MQTT)
        {
            if (len > 0 && len < sizeof(MqttRxBuffer))
            {
                WCHNET_SocketRecv(socketid, MqttRxBuffer, &len);
                MqttRxLen = (uint16_t)len;
            }
            else
            {
                WCHNET_SocketRecv(socketid, NULL, &len);
            }
        }
        else
        {
            // UDP 收包暂时丢弃
            WCHNET_SocketRecv(socketid, NULL, &len);
        }
    }

    if (intstat & SINT_STAT_CONNECT)
    {
        if (socketid == SocketId_MQTT) {
            printf("MQTT TCP Connected!\r\n");
            MQTT_Conn_Flag = 1;
        }
    }

    if (intstat & SINT_STAT_DISCONNECT)
    {
        if (socketid == SocketId_MQTT) {
            printf("MQTT TCP Disconnected!\r\n");
            MQTT_Conn_Flag = 0;
        }
    }
}

void WCHNET_HandleGlobalInt(void)
{
    uint8_t intstat;
    uint16_t i;
    uint8_t socketint;

    intstat = WCHNET_GetGlobalInt();
    if (intstat & GINT_STAT_PHY_CHANGE)
    {
        i = WCHNET_GetPHYStatus();
        if (i & PHY_Linked_Status) printf("PHY Link Success\r\n");
    }

    if (intstat & GINT_STAT_SOCKET)
    {
        for (i = 0; i < WCHNET_MAX_SOCKET_NUM; i++)
        {
            socketint = WCHNET_GetSocketInt(i);
            if (socketint) WCHNET_HandleSockInt(i, socketint);
        }
    }
}

