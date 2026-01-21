#ifndef __UDP_H__
#define __UDP_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 这些变量在 UDP.c 里定义，这里给 extern 声明
extern uint8_t MACAddr[6];
extern uint8_t IPAddr[4];
extern uint8_t GWIPAddr[4];
extern uint8_t IPMask[4];

extern uint8_t SocketId_UDP;
extern uint8_t SocketId_MQTT;
extern uint8_t MQTT_Conn_Flag;

// 创建 socket
void WCHNET_CreateUdpSocket(void);
void WCHNET_CreateMqttSocket(void);

// ====================== UDP 发送接口 ======================
// 你的网络规划：
//   - 主板：10.1.3.x
//   - 节点：10.1.4.x
// 本机（CH32）：10.1.5.1

// 目标设备 UDP 端口（根据你的实际接收端修改）
// 说明：原工程里 CreateUdpSocket() 的 DesPort=1000，注释也写了主板监听端口=1000。
// 因此这里默认用 1000。
#define UDP_DEFAULT_DPORT  1000

// 主板单点：10.1.3.{mb_id}
void UDP_SendTo_DynamicIP(uint8_t mb_id, const char *data, uint32_t len);

// 节点单点：10.1.4.{node_id}
void UDP_SendTo_NodeIP(uint8_t node_id, const char *data, uint32_t len);

// 指定 IP/端口
void UDP_SendTo_IP(const uint8_t ip[4], uint16_t port, const char *data, uint32_t len);

// 群发/单点封装
// 说明：群发默认按 1..UDP_BROADCAST_MAX_MB_ID 逐个发送（更可靠，不依赖网络广播）
#define UDP_BROADCAST_MAX_MB_ID  20   // <<< 这里改成你实际主板数量/最大 mb_id
void UDP_SendByTarget(uint8_t broadcast, uint8_t mb_id, const char *data, uint32_t len);

// misc
void mStopIfError(uint8_t iError);
void WCHNET_HandleGlobalInt(void);

#ifdef __cplusplus
}
#endif

#endif
