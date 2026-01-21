#ifndef __APP_LOGIC_H__
#define __APP_LOGIC_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_MAX_SCHEDULE_ITEMS   8
#define APP_TIME_STR_LEN         6    // "HH:MM" + '\0'
#define APP_SYS_TIME_LEN         20   // "YYYY-MM-DD HH:MM:SS" (19) + '\0'

typedef enum {
    TASK_MODE_UNKNOWN = 0,
    TASK_MODE_INSPECTION,
    TASK_MODE_MANUAL
} task_mode_t;

typedef struct
{
    char sys_time[APP_SYS_TIME_LEN];

    uint8_t broadcast;      // 1=群发 0=单点
    uint8_t mb_id;          // 1..254 (单点时有效)
    uint16_t bs_id;
    uint16_t sensor_id;

    task_mode_t mode;

    // inspection
    uint8_t schedule_count;
    char schedule[APP_MAX_SCHEDULE_ITEMS][APP_TIME_STR_LEN];

    // manual
    char start_at[APP_TIME_STR_LEN];
    uint16_t duration_min;
} ParsedData_t;

// ================= JSON 原样转发：只解析目标地址 =================
typedef enum {
    JSON_FWD_NONE = 0,
    JSON_FWD_BROADCAST,
    JSON_FWD_MB_ID,
    JSON_FWD_IP
} json_fwd_target_type_t;

typedef struct {
    json_fwd_target_type_t type;
    uint8_t mb_id;      // type==JSON_FWD_MB_ID 时有效 (1..254)
    uint8_t ip[4];      // type==JSON_FWD_IP 时有效
    uint16_t port;      // type==JSON_FWD_IP 时有效；未给则用 UDP_DEFAULT_DPORT
} JsonForwardTarget_t;

/**
 * @brief  解析任意 JSON 字符串，只提取“要发到哪个主板/哪个 IP”
 *
 * 兼容字段（优先级从高到低）：
 *  - target: { broadcast, mb_id/node, ip/dst_ip/board_ip/target_ip, port }
 *  - broadcast
 *  - ip/dst_ip/board_ip/target_ip + port
 *  - node/mb_id
 *  - cmd=="sync" 且未指定 node/ip 时 -> 广播
 */
int Parse_Forward_Target(const char *json, JsonForwardTarget_t *out_tgt);

/**
 * @brief  解析 ThingsBoard 下发 JSON（支持 inspection/manual 两种）
 * @return 0 成功，-1 失败
 */
int Parse_Mqtt_Json(const char *json, ParsedData_t *out_data);

/**
 * @brief  将解析结果拼成“串口字符串”格式（最终用于 UDP payload）
 * @return 返回实际长度（不含末尾 '\0'），<=0 表示失败
 */
int Build_Serial_Udp_String(const ParsedData_t *data, char *out_buf, uint16_t out_len);

// ============================================================================
// =================== NanoPi 串口下发：daily/temp 两种 ========================
// 用户给的 NanoPi JSON:
//  1) daily: params.on / params.off
//  2) temp : params.start / params.dur_min
// ============================================================================

typedef enum {
    NANO_MODE_UNKNOWN = 0,
    NANO_MODE_DAILY,
    NANO_MODE_TEMP
} nano_mode_t;

typedef struct {
    char time_str[APP_SYS_TIME_LEN];   // "YYYY-MM-DD HH:MM:SS"，用于校时（可为空）

    // 设备编号（原样保留字符串，带前导 0）
    char mb_id[5]; // 4位 + '\0'
    char bs_id[4]; // 3位 + '\0'
    char sn_id[3]; // 2位 + '\0'

    nano_mode_t mode;

    // daily
    char on[APP_TIME_STR_LEN];
    char off[APP_TIME_STR_LEN];

    // temp
    char start[APP_TIME_STR_LEN];
    uint16_t dur_min;
} NanoJson_t;

/**
 * @brief  解析 NanoPi 串口 JSON（daily/temp）
 * @return 0 成功，-1 失败
 */
int Parse_NanoPi_Json(const char *json, NanoJson_t *out);

/**
 * @brief  拼一个下发给主板/节点的 UDP 命令 JSON（on/off）
 *         例：{"cmd":"on","dev":{"mb_id":"0001","bs_id":"001","sn_id":"01"}}
 * @return 实际长度（不含 '\0'），<0 失败
 */
int Build_Action_Json(const NanoJson_t *task, const char *action, char *out_buf, uint16_t out_len);

/**
 * @brief  NanoPi JSON -> 校时 + 更新调度表
 * @return 0 成功，-1 失败
 */
int AppScheduler_HandleJson(const char *json);

/**
 * @brief  调度器 tick：建议在 main while(1) 里周期调用
 * @param  now_ms - 毫秒 tick（用 g_net_tick_ms 即可）
 */
void AppScheduler_Tick(uint32_t now_ms);

/**
 * @brief  调度器初始化
 */
void AppScheduler_Init(void);

/**
 * @brief  读取外部 RTC 当前时间并格式化为字符串
 * @param  out_buf  输出缓冲区
 * @param  out_len  缓冲区长度
 * @return 0 成功，-1 失败
 *
 * 输出格式："YYYY-MM-DD HH:MM:SS"
 */
int AppRTC_GetNowString(char *out_buf, uint16_t out_len);

int ds1307_pro(void);

#ifdef __cplusplus
}
#endif

#endif
