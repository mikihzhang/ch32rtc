#include "app_logic.h"
#include "GPIO.h"   // PB10/PB11 definitions
#include "debug.h"  // Delay_Us
#include <string.h>
#include <stdio.h>
#include "ch32v20x.h"
#include "cJSON.h"
#include "UDP.h"

static int copy_str(char *dst, uint16_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return -1;
    if (!src) { dst[0] = '\0'; return 0; }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
    return 0;
}

static int parse_ipv4_str(const char *s, uint8_t out_ip[4])
{
    if (!s || !out_ip) return -1;

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;

    int a, b, c, d;
    char tail = 0;
    if (sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) < 4) return -1;
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return -1;
    out_ip[0] = (uint8_t)a;
    out_ip[1] = (uint8_t)b;
    out_ip[2] = (uint8_t)c;
    out_ip[3] = (uint8_t)d;
    return 0;
}

static cJSON* get_first_key(cJSON *obj, const char *k1, const char *k2, const char *k3, const char *k4)
{
    cJSON *it = NULL;
    if (obj && k1) { it = cJSON_GetObjectItem(obj, k1); if (it) return it; }
    if (obj && k2) { it = cJSON_GetObjectItem(obj, k2); if (it) return it; }
    if (obj && k3) { it = cJSON_GetObjectItem(obj, k3); if (it) return it; }
    if (obj && k4) { it = cJSON_GetObjectItem(obj, k4); if (it) return it; }
    return NULL;
}

int Parse_Forward_Target(const char *json, JsonForwardTarget_t *out_tgt)
{
    if (!json || !out_tgt) return -1;
    memset(out_tgt, 0, sizeof(*out_tgt));
    out_tgt->type = JSON_FWD_NONE;

    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;

    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");

    cJSON *tgt = cJSON_GetObjectItem(root, "target");
    cJSON *objs[2] = { cJSON_IsObject(tgt) ? tgt : NULL, root };

    for (uint8_t oi = 0; oi < 2; oi++)
    {
        cJSON *search_obj = objs[oi];
        if (!search_obj) continue;

        // 1) broadcast
        {
            cJSON *bc = cJSON_GetObjectItem(search_obj, "broadcast");
            if (cJSON_IsTrue(bc)) {
                out_tgt->type = JSON_FWD_BROADCAST;
                cJSON_Delete(root);
                return 0;
            }
        }

        // 2) mb: can be number (mb_id) or string (ip or mb_id)
        {
            cJSON *mb = cJSON_GetObjectItem(search_obj, "mb");
            if (!mb) mb = cJSON_GetObjectItem(search_obj, "MB");

            if (cJSON_IsNumber(mb) && mb->valueint > 0 && mb->valueint <= 254) {
                out_tgt->type = JSON_FWD_MB_ID;
                out_tgt->mb_id = (uint8_t)mb->valueint;
                cJSON_Delete(root);
                return 0;
            }

            if (cJSON_IsString(mb) && mb->valuestring) {
                // try ip first
                if (parse_ipv4_str(mb->valuestring, out_tgt->ip) == 0) {
                    out_tgt->type = JSON_FWD_IP;
                    cJSON *port = get_first_key(search_obj, "port", "dst_port", "udp_port", NULL);
                    if (cJSON_IsNumber(port) && port->valueint > 0 && port->valueint <= 65535) {
                        out_tgt->port = (uint16_t)port->valueint;
                    } else {
                        out_tgt->port = UDP_DEFAULT_DPORT;
                    }
                    cJSON_Delete(root);
                    return 0;
                }

                // then try mb_id in string
                int id = 0;
                char tail = 0;
                if (sscanf(mb->valuestring, " %d %c", &id, &tail) == 1 && id > 0 && id <= 254) {
                    out_tgt->type = JSON_FWD_MB_ID;
                    out_tgt->mb_id = (uint8_t)id;
                    cJSON_Delete(root);
                    return 0;
                }
            }
        }

        // 3) ip (other common field names)
        {
            cJSON *ip = get_first_key(search_obj, "ip", "dst_ip", "board_ip", "target_ip");
            if (cJSON_IsString(ip) && ip->valuestring) {
                if (parse_ipv4_str(ip->valuestring, out_tgt->ip) == 0) {
                    out_tgt->type = JSON_FWD_IP;
                    cJSON *port = get_first_key(search_obj, "port", "dst_port", "udp_port", NULL);
                    if (cJSON_IsNumber(port) && port->valueint > 0 && port->valueint <= 65535) {
                        out_tgt->port = (uint16_t)port->valueint;
                    } else {
                        out_tgt->port = UDP_DEFAULT_DPORT;
                    }
                    cJSON_Delete(root);
                    return 0;
                }
            }
        }

        // 4) id fallback: node/mb_id/id (numeric)
        {
            cJSON *id = get_first_key(search_obj, "node", "mb_id", "id", NULL);
            if (cJSON_IsNumber(id) && id->valueint > 0 && id->valueint <= 254) {
                out_tgt->type = JSON_FWD_MB_ID;
                out_tgt->mb_id = (uint8_t)id->valueint;
                cJSON_Delete(root);
                return 0;
            }
        }
    }

    // Special: cmd=="sync" and no explicit target => broadcast
    if (cJSON_IsString(cmd) && cmd->valuestring && strcmp(cmd->valuestring, "sync") == 0) {
        out_tgt->type = JSON_FWD_BROADCAST;
        cJSON_Delete(root);
        return 0;
    }

    cJSON_Delete(root);
    return -1;
}


int Parse_Mqtt_Json(const char *json, ParsedData_t *out_data)
{
    cJSON *root = NULL;
    cJSON *target = NULL;
    cJSON *task = NULL;

    if (!json || !out_data) return -1;
    memset(out_data, 0, sizeof(*out_data));

    root = cJSON_Parse(json);
    if (!root) return -1;

    // sys_time (可选)
    {
        cJSON *item = cJSON_GetObjectItem(root, "sys_time");
        if (cJSON_IsString(item) && item->valuestring) {
            copy_str(out_data->sys_time, sizeof(out_data->sys_time), item->valuestring);
        } else {
            out_data->sys_time[0] = '\0';
        }
    }

    // ------------- 尝试解析新格式：target/task -------------
    target = cJSON_GetObjectItem(root, "target");
    task   = cJSON_GetObjectItem(root, "task");

    if (cJSON_IsObject(target) && cJSON_IsObject(task))
    {
        // broadcast
        {
            cJSON *item = cJSON_GetObjectItem(target, "broadcast");
            out_data->broadcast = cJSON_IsTrue(item) ? 1 : 0;
        }

        // mb_id / bs_id / sensor_id
        {
            cJSON *mb = cJSON_GetObjectItem(target, "mb_id");
            cJSON *bs = cJSON_GetObjectItem(target, "bs_id");
            cJSON *sn = cJSON_GetObjectItem(target, "sensor_id");

            if (!cJSON_IsNumber(mb) || !cJSON_IsNumber(bs) || !cJSON_IsNumber(sn)) {
                cJSON_Delete(root);
                return -1;
            }

            out_data->mb_id = (uint8_t)mb->valueint;
            out_data->bs_id = (uint16_t)bs->valueint;
            out_data->sensor_id = (uint16_t)sn->valueint;
        }

        // mode
        {
            cJSON *mode = cJSON_GetObjectItem(task, "mode");
            if (!cJSON_IsString(mode) || !mode->valuestring) {
                cJSON_Delete(root);
                return -1;
            }

            if (strcmp(mode->valuestring, "inspection") == 0) {
                out_data->mode = TASK_MODE_INSPECTION;
            } else if (strcmp(mode->valuestring, "manual") == 0) {
                out_data->mode = TASK_MODE_MANUAL;
            } else {
                out_data->mode = TASK_MODE_UNKNOWN;
            }
        }

        // inspection: schedule[]
        if (out_data->mode == TASK_MODE_INSPECTION)
        {
            cJSON *schedule = cJSON_GetObjectItem(task, "schedule");
            if (!cJSON_IsArray(schedule)) {
                cJSON_Delete(root);
                return -1;
            }

            int n = cJSON_GetArraySize(schedule);
            if (n < 1) {
                cJSON_Delete(root);
                return -1;
            }

            if (n > APP_MAX_SCHEDULE_ITEMS) n = APP_MAX_SCHEDULE_ITEMS;
            out_data->schedule_count = (uint8_t)n;

            for (int i = 0; i < n; i++) {
                cJSON *t = cJSON_GetArrayItem(schedule, i);
                if (cJSON_IsString(t) && t->valuestring) {
                    copy_str(out_data->schedule[i], APP_TIME_STR_LEN, t->valuestring);
                } else {
                    out_data->schedule[i][0] = '\0';
                }
            }
        }

        // manual: config.start_at + duration
        if (out_data->mode == TASK_MODE_MANUAL)
        {
            cJSON *config = cJSON_GetObjectItem(task, "config");
            if (!cJSON_IsObject(config)) {
                cJSON_Delete(root);
                return -1;
            }

            cJSON *start_at = cJSON_GetObjectItem(config, "start_at");
            cJSON *duration = cJSON_GetObjectItem(config, "duration");

            if (!cJSON_IsString(start_at) || !start_at->valuestring || !cJSON_IsNumber(duration)) {
                cJSON_Delete(root);
                return -1;
            }

            copy_str(out_data->start_at, APP_TIME_STR_LEN, start_at->valuestring);
            out_data->duration_min = (uint16_t)duration->valueint;
        }

        cJSON_Delete(root);
        return 0;
    }

    // ------------- 回退解析老格式：根节点扁平 -------------
    {
        cJSON *mb = cJSON_GetObjectItem(root, "mb_id");
        cJSON *bs = cJSON_GetObjectItem(root, "bs_id");
        cJSON *sn = cJSON_GetObjectItem(root, "sensor_id");
        cJSON *mode = cJSON_GetObjectItem(root, "mode");

        if (!cJSON_IsNumber(mb) || !cJSON_IsNumber(bs) || !cJSON_IsNumber(sn) || !cJSON_IsString(mode)) {
            cJSON_Delete(root);
            return -1;
        }

        out_data->broadcast = 0; // 老格式默认单点
        out_data->mb_id = (uint8_t)mb->valueint;
        out_data->bs_id = (uint16_t)bs->valueint;
        out_data->sensor_id = (uint16_t)sn->valueint;

        // mode 映射：你现在发的是 "A"
        // 你可以自定义：A=inspection, B=manual
        if (strcmp(mode->valuestring, "A") == 0) {
            out_data->mode = TASK_MODE_INSPECTION;
            // 老格式没有 schedule，给个默认值（否则 Build 会 SCH=00:00）
            out_data->schedule_count = 1;
            copy_str(out_data->schedule[0], APP_TIME_STR_LEN, "12:00");
        } else if (strcmp(mode->valuestring, "B") == 0) {
            out_data->mode = TASK_MODE_MANUAL;
            copy_str(out_data->start_at, APP_TIME_STR_LEN, "12:00");
            out_data->duration_min = 45;
        } else {
            out_data->mode = TASK_MODE_UNKNOWN;
        }

        cJSON_Delete(root);
        return 0;
    }
}


int Build_Serial_Udp_String(const ParsedData_t *data, char *out_buf, uint16_t out_len)
{
    if (!data || !out_buf || out_len == 0) return -1;

    const char *mode_str = "UNK";
    if (data->mode == TASK_MODE_INSPECTION) mode_str = "INS";
    else if (data->mode == TASK_MODE_MANUAL) mode_str = "MAN";

    int used = 0;
    int n = 0;

    // Header
    n = snprintf(out_buf, out_len,
                 "SYS=%s,BC=%u,MB=%u,BS=%u,SN=%u,MODE=%s",
                 (data->sys_time[0] ? data->sys_time : "0"),
                 data->broadcast,
                 data->mb_id,
                 (unsigned)data->bs_id,
                 (unsigned)data->sensor_id,
                 mode_str);
    if (n < 0) return -1;
    if (n >= (int)out_len) return (int)(out_len - 1);
    used = n;

    // Payload by mode
    if (data->mode == TASK_MODE_INSPECTION)
    {
        n = snprintf(out_buf + used, out_len - used, ",SCH=");
        if (n < 0) return -1;
        if (n >= (int)(out_len - used)) return (int)(out_len - 1);
        used += n;

        for (uint8_t i = 0; i < data->schedule_count; i++)
        {
            const char *t = data->schedule[i][0] ? data->schedule[i] : "00:00";
            n = snprintf(out_buf + used, out_len - used, "%s%s", (i == 0 ? "" : "|"), t);
            if (n < 0) return -1;
            if (n >= (int)(out_len - used)) return (int)(out_len - 1);
            used += n;
        }
    }
    else if (data->mode == TASK_MODE_MANUAL)
    {
        n = snprintf(out_buf + used, out_len - used,
                     ",START=%s,DUR=%u",
                     (data->start_at[0] ? data->start_at : "00:00"),
                     (unsigned)data->duration_min);
        if (n < 0) return -1;
        if (n >= (int)(out_len - used)) return (int)(out_len - 1);
        used += n;
    }

    // Trailer
    n = snprintf(out_buf + used, out_len - used, "\r\n");
    if (n < 0) return -1;
    if (n >= (int)(out_len - used)) return (int)(out_len - 1);
    used += n;

    return used;
}


// ============================================================================
// ===================== NanoPi JSON (USART2) + RTC + Scheduler ===============
// ============================================================================

#ifndef APP_SCHED_MAX_ITEMS
#define APP_SCHED_MAX_ITEMS  16
#endif

// 选择“节点 IP”的来源：
//  1: 10.1.4.{bs_id}
//  0: 10.1.4.{sn_id}
#ifndef APP_NODE_IP_FROM_BS_ID
#define APP_NODE_IP_FROM_BS_ID  1
#endif

// 外部 RTC 型号：默认按 DS3231/DS1307 寄存器布局 (0x68)
#ifndef EXT_RTC_I2C
#define EXT_RTC_I2C            I2C2
#endif

#ifndef EXT_RTC_ADDR_7BIT
#define EXT_RTC_ADDR_7BIT      0x68
#endif

// IMPORTANT (WCH SPL): I2C_Send7bitAddress() expects a 7-bit address (0x68),
// and the R/W direction is provided by the I2C_Direction_xxx argument.

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
} DateTime_t;

typedef struct {
    uint8_t used;

    // 原样保留字符串（便于下发时带前导 0）
    char mb_id[5];
    char bs_id[4];
    char sn_id[3];

    uint8_t mb_host;     // 1..254
    uint8_t node_host;   // 1..254

    nano_mode_t mode;

    // daily
    uint16_t on_minute;
    uint16_t off_minute;
    int32_t last_on_day;
    int32_t last_off_day;

    // temp
    uint32_t start_ts;
    uint32_t end_ts;
    uint8_t temp_started;
    uint8_t temp_finished;
} SchedItem_t;

static SchedItem_t g_sched[APP_SCHED_MAX_ITEMS];
static uint32_t g_sched_last_poll_ms = 0;
static DateTime_t g_last_rtc_dt;
static uint8_t g_rtc_inited = 0;

// ------------------------- small utils -------------------------
static int copy_fixed(char *dst, uint16_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return -1;
    if (!src) { dst[0] = '\0'; return 0; }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
    return 0;
}

static int parse_u8_dec(const char *s, uint8_t *out)
{
    if (!s || !out) return -1;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    int v = 0;
    char tail = 0;
    if (sscanf(s, "%d%c", &v, &tail) < 1) return -1;
    if (v < 0 || v > 254) return -1;
    *out = (uint8_t)v;
    return 0;
}

static int parse_hhmm_to_minute(const char *s, uint16_t *out_min)
{
    if (!s || !out_min) return -1;
    int hh = 0, mm = 0;
    char tail = 0;
    if (sscanf(s, "%d:%d%c", &hh, &mm, &tail) < 2) return -1;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
    *out_min = (uint16_t)(hh * 60 + mm);
    return 0;
}

static int parse_datetime(const char *s, DateTime_t *out)
{
    if (!s || !out) return -1;
    int Y,M,D,h,m,sec;
    if (sscanf(s, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return -1;
    if (Y < 2000 || Y > 2099) return -1;
    if (M < 1 || M > 12) return -1;
    if (D < 1 || D > 31) return -1;
    if (h < 0 || h > 23) return -1;
    if (m < 0 || m > 59) return -1;
    if (sec < 0 || sec > 59) return -1;
    out->year = (uint16_t)Y;
    out->month = (uint8_t)M;
    out->day = (uint8_t)D;
    out->hour = (uint8_t)h;
    out->min = (uint8_t)m;
    out->sec = (uint8_t)sec;
    return 0;
}

static int is_leap(int y)
{
    return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
}

static uint8_t days_in_month(int y, int m)
{
    static const uint8_t mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m < 1 || m > 12) return 31;
    if (m == 2 && is_leap(y)) return 29;
    return mdays[m-1];
}

// days since 2000-01-01
static int32_t date_to_days(uint16_t year, uint8_t month, uint8_t day)
{
    int32_t days = 0;
    for (int y = 2000; y < (int)year; y++) {
        days += is_leap(y) ? 366 : 365;
    }
    for (int m = 1; m < (int)month; m++) {
        days += days_in_month((int)year, m);
    }
    days += (int32_t)(day - 1);
    return days;
}

static uint32_t dt_to_seconds(const DateTime_t *dt)
{
    if (!dt) return 0;
    int32_t days = date_to_days(dt->year, dt->month, dt->day);
    return (uint32_t)(days * 86400 + (int32_t)dt->hour * 3600 + (int32_t)dt->min * 60 + dt->sec);
}

// ------------------------- I2C helpers -------------------------
static uint8_t bcd2bin(uint8_t bcd)
{
    return (uint8_t)((bcd >> 4) * 10 + (bcd & 0x0F));
}

static uint8_t bin2bcd(uint8_t bin)
{
    return (uint8_t)(((bin / 10) << 4) | (bin % 10));
}

static int i2c_wait_event(uint32_t event, uint32_t timeout)
{
    while (timeout--) {
        if (I2C_CheckEvent(EXT_RTC_I2C, event) == READY) return 0;
    }
    printf("I2C wait event timeout: 0x%08lx, STAR1=0x%08lx STAR2=0x%08lx\r\n",
           event, EXT_RTC_I2C->STAR1, EXT_RTC_I2C->STAR2);
    return -1;
}

// Clear ACK failure and bring peripheral back to a sane state.
static void i2c_abort_recover(void)
{
    // Clear AF (ACK failure) if set
    if (EXT_RTC_I2C->STAR1 & 0x0400) {
        // AF bit in STAR1
        EXT_RTC_I2C->STAR1 &= ~0x0400;
    }

    // Generate STOP to release the bus (best-effort)
    I2C_GenerateSTOP(EXT_RTC_I2C, ENABLE);
    Delay_Us(10);

    // Soft reset I2C (helps if BUSY/AF stuck)
    I2C_SoftwareResetCmd(EXT_RTC_I2C, ENABLE);
    Delay_Us(10);
    I2C_SoftwareResetCmd(EXT_RTC_I2C, DISABLE);
    Delay_Us(10);

    // Re-enable peripheral + ACK
    I2C_Cmd(EXT_RTC_I2C, ENABLE);
    I2C_AcknowledgeConfig(EXT_RTC_I2C, ENABLE);
}

// If SCL/SDA are held LOW (no pull-up / wiring / device stuck), hardware START will never set SB.
// Try a simple bus recovery by toggling SCL as GPIO and issuing a STOP.
static void i2c2_bus_recover_if_needed(void)
{
    // Only intended for I2C2 on PB10/PB11 in this project
    if (EXT_RTC_I2C != I2C2) return;

    // Read current line levels
    uint8_t scl = GPIO_ReadInputDataBit(GPIOB, ADC_SCL) ? 1 : 0;
    uint8_t sda = GPIO_ReadInputDataBit(GPIOB, ADC_SDA) ? 1 : 0;

    if (scl && sda) return; // bus looks idle

    printf("[I2C] bus not idle (SCL=%d SDA=%d), try recover...\r\n", scl, sda);

    GPIO_InitTypeDef g = {0};

    // Temporarily disable I2C2 so GPIO can control pins
    I2C_Cmd(I2C2, DISABLE);

    // Configure PB10/PB11 as open-drain outputs
    g.GPIO_Pin = ADC_SCL | ADC_SDA;
    g.GPIO_Mode = GPIO_Mode_Out_OD;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &g);

    // Release both lines (drive high -> OD release)
    GPIO_SetBits(GPIOB, ADC_SCL | ADC_SDA);
    Delay_Us(5);

    // Clock out 9 pulses on SCL to free a stuck slave
    for (int i = 0; i < 9; i++) {
        GPIO_ResetBits(GPIOB, ADC_SCL);
        Delay_Us(5);
        GPIO_SetBits(GPIOB, ADC_SCL);
        Delay_Us(5);
    }

    // Issue a STOP: SDA low while SCL high, then SDA high
    GPIO_SetBits(GPIOB, ADC_SCL);
    Delay_Us(5);
    GPIO_ResetBits(GPIOB, ADC_SDA);
    Delay_Us(5);
    GPIO_SetBits(GPIOB, ADC_SDA);
    Delay_Us(5);

    // Restore PB10/PB11 to I2C AF open-drain
    g.GPIO_Mode = GPIO_Mode_AF_OD;
    GPIO_Init(GPIOB, &g);
    Delay_Us(5);

    // Re-enable I2C2
    I2C_Cmd(I2C2, ENABLE);
    I2C_AcknowledgeConfig(I2C2, ENABLE);

    // Clear potential stuck flags
    i2c_abort_recover();
}


static int rtc_write_regs(uint8_t reg, const uint8_t *data, uint8_t len)
{
    if (!data || len == 0) return -1;

    // Try bus recovery if lines are stuck low
    i2c2_bus_recover_if_needed();

    // wait not busy (simple)
    uint32_t to = 100000;
    while ((EXT_RTC_I2C->STAR2 & I2C_STAR2_BUSY) && to--) {}

    I2C_GenerateSTART(EXT_RTC_I2C, ENABLE);
    if (i2c_wait_event(I2C_EVENT_MASTER_MODE_SELECT, 200000) != 0) goto err;

    I2C_Send7bitAddress(EXT_RTC_I2C, EXT_RTC_ADDR_7BIT, I2C_Direction_Transmitter);
    if (i2c_wait_event(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED, 200000) != 0) goto err;

    I2C_SendData(EXT_RTC_I2C, reg);
    if (i2c_wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED, 200000) != 0) goto err;

    for (uint8_t i = 0; i < len; i++) {
        I2C_SendData(EXT_RTC_I2C, data[i]);
        if (i2c_wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED, 200000) != 0) goto err;
    }

    I2C_GenerateSTOP(EXT_RTC_I2C, ENABLE);
    return 0;

err:
    i2c_abort_recover();
    return -1;
}

static int rtc_read_regs(uint8_t reg, uint8_t *out, uint8_t len)
{
    if (!out || len == 0) return -1;

    // Try bus recovery if lines are stuck low
    i2c2_bus_recover_if_needed();

    uint32_t to = 100000;
    while ((EXT_RTC_I2C->STAR2 & I2C_STAR2_BUSY) && to--) {}

    // write reg pointer
    I2C_GenerateSTART(EXT_RTC_I2C, ENABLE);
    if (i2c_wait_event(I2C_EVENT_MASTER_MODE_SELECT, 200000) != 0) goto err;

    I2C_Send7bitAddress(EXT_RTC_I2C, EXT_RTC_ADDR_7BIT, I2C_Direction_Transmitter);
    if (i2c_wait_event(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED, 200000) != 0) goto err;

    I2C_SendData(EXT_RTC_I2C, reg);
    if (i2c_wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED, 200000) != 0) goto err;

    // repeated start
    I2C_GenerateSTART(EXT_RTC_I2C, ENABLE);
    if (i2c_wait_event(I2C_EVENT_MASTER_MODE_SELECT, 200000) != 0) goto err;

    I2C_Send7bitAddress(EXT_RTC_I2C, EXT_RTC_ADDR_7BIT, I2C_Direction_Receiver);
    if (i2c_wait_event(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED, 200000) != 0) goto err;

    if (len == 1) {
        I2C_AcknowledgeConfig(EXT_RTC_I2C, DISABLE);
        I2C_GenerateSTOP(EXT_RTC_I2C, ENABLE);
        if (i2c_wait_event(I2C_EVENT_MASTER_BYTE_RECEIVED, 200000) != 0) goto err;
        out[0] = I2C_ReceiveData(EXT_RTC_I2C);
    } else {
        I2C_AcknowledgeConfig(EXT_RTC_I2C, ENABLE);
        for (uint8_t i = 0; i < len; i++) {
            if (i == (uint8_t)(len - 1)) {
                I2C_AcknowledgeConfig(EXT_RTC_I2C, DISABLE);
                I2C_GenerateSTOP(EXT_RTC_I2C, ENABLE);
            }
            if (i2c_wait_event(I2C_EVENT_MASTER_BYTE_RECEIVED, 200000) != 0) goto err;
            out[i] = I2C_ReceiveData(EXT_RTC_I2C);
        }
    }

    // restore ACK
    I2C_AcknowledgeConfig(EXT_RTC_I2C, ENABLE);
    return 0;

err:
    i2c_abort_recover();
    return -1;
}

static int rtc_set_datetime(const DateTime_t *dt)
{
    if (!dt) return -1;

    uint8_t regs[7];
    regs[0] = bin2bcd(dt->sec);
    regs[1] = bin2bcd(dt->min);
    regs[2] = bin2bcd(dt->hour);    // 24h
    regs[3] = 1;                    // weekday (not used)
    regs[4] = bin2bcd(dt->day);
    regs[5] = bin2bcd(dt->month);
    regs[6] = bin2bcd((uint8_t)(dt->year - 2000));

    int rc = rtc_write_regs(0x00, regs, 7);
    if (rc == 0) {
        g_rtc_inited = 1;
        g_last_rtc_dt = *dt;
    }
    return rc;
}

static int rtc_get_datetime(DateTime_t *out)
{
    if (!out) return -1;

    uint8_t regs[7];
    if (rtc_read_regs(0x00, regs, 7) != 0) return -1;

    out->sec   = bcd2bin(regs[0] & 0x7F);
    out->min   = bcd2bin(regs[1] & 0x7F);
    out->hour  = bcd2bin(regs[2] & 0x3F);
    out->day   = bcd2bin(regs[4] & 0x3F);
    out->month = bcd2bin(regs[5] & 0x1F);
    out->year  = 2000 + bcd2bin(regs[6]);

    g_last_rtc_dt = *out;
    g_rtc_inited = 1;
    return 0;
}

// ====================== Public helper: RTC -> string =======================
int AppRTC_GetNowString(char *out_buf, uint16_t out_len)
{
    if (!out_buf || out_len < 20) return -1;

    DateTime_t dt;
    if (rtc_get_datetime(&dt) != 0) return -1;

    // "YYYY-MM-DD HH:MM:SS" = 19 chars
    (void)snprintf(out_buf, out_len, "%04u-%02u-%02u %02u:%02u:%02u",
                   (unsigned)dt.year,
                   (unsigned)dt.month,
                   (unsigned)dt.day,
                   (unsigned)dt.hour,
                   (unsigned)dt.min,
                   (unsigned)dt.sec);
    return 0;
}

// ------------------------- NanoPi JSON parse/build -------------------------

int Parse_NanoPi_Json(const char *json, NanoJson_t *out)
{
    if (!json || !out) return -1;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;

    // time
    cJSON *time = cJSON_GetObjectItem(root, "time");
    if (cJSON_IsString(time) && time->valuestring) {
        copy_fixed(out->time_str, sizeof(out->time_str), time->valuestring);
    } else {
        out->time_str[0] = '\0';
    }

    // dev
    cJSON *dev = cJSON_GetObjectItem(root, "dev");
    if (!cJSON_IsObject(dev)) { cJSON_Delete(root); return -1; }

    cJSON *mb = cJSON_GetObjectItem(dev, "mb_id");
    cJSON *bs = cJSON_GetObjectItem(dev, "bs_id");
    cJSON *sn = cJSON_GetObjectItem(dev, "sn_id");

    if (!cJSON_IsString(mb) || !mb->valuestring) { cJSON_Delete(root); return -1; }
    if (!cJSON_IsString(bs) || !bs->valuestring) { cJSON_Delete(root); return -1; }
    if (!cJSON_IsString(sn) || !sn->valuestring) { cJSON_Delete(root); return -1; }

    copy_fixed(out->mb_id, sizeof(out->mb_id), mb->valuestring);
    copy_fixed(out->bs_id, sizeof(out->bs_id), bs->valuestring);
    copy_fixed(out->sn_id, sizeof(out->sn_id), sn->valuestring);

    // mode
    cJSON *mode = cJSON_GetObjectItem(root, "mode");
    if (!cJSON_IsString(mode) || !mode->valuestring) { cJSON_Delete(root); return -1; }

    if (strcmp(mode->valuestring, "daily") == 0) out->mode = NANO_MODE_DAILY;
    else if (strcmp(mode->valuestring, "temp") == 0) out->mode = NANO_MODE_TEMP;
    else out->mode = NANO_MODE_UNKNOWN;

    // params
    cJSON *params = cJSON_GetObjectItem(root, "params");
    if (!cJSON_IsObject(params)) { cJSON_Delete(root); return -1; }

    if (out->mode == NANO_MODE_DAILY)
    {
        cJSON *on  = cJSON_GetObjectItem(params, "on");
        cJSON *off = cJSON_GetObjectItem(params, "off");
        if (!cJSON_IsString(on) || !on->valuestring) { cJSON_Delete(root); return -1; }
        if (!cJSON_IsString(off) || !off->valuestring) { cJSON_Delete(root); return -1; }

        copy_fixed(out->on, sizeof(out->on), on->valuestring);
        copy_fixed(out->off, sizeof(out->off), off->valuestring);
    }
    else if (out->mode == NANO_MODE_TEMP)
    {
        cJSON *start = cJSON_GetObjectItem(params, "start");
        cJSON *dur   = cJSON_GetObjectItem(params, "dur_min");
        if (!cJSON_IsString(start) || !start->valuestring) { cJSON_Delete(root); return -1; }
        if (!cJSON_IsNumber(dur)) { cJSON_Delete(root); return -1; }

        copy_fixed(out->start, sizeof(out->start), start->valuestring);
        out->dur_min = (uint16_t)dur->valueint;
    }
    else
    {
        cJSON_Delete(root);
        return -1;
    }

    cJSON_Delete(root);
    return 0;
}

int Build_Action_Json(const NanoJson_t *task, const char *action, char *out_buf, uint16_t out_len)
{
    if (!task || !action || !out_buf || out_len == 0) return -1;

    int n = snprintf(out_buf, out_len,
                     "{\"cmd\":\"%s\",\"dev\":{\"mb_id\":\"%s\",\"bs_id\":\"%s\",\"sn_id\":\"%s\"}}\r\n",
                     action,
                     task->mb_id[0] ? task->mb_id : "0000",
                     task->bs_id[0] ? task->bs_id : "000",
                     task->sn_id[0] ? task->sn_id : "00");
    if (n < 0) return -1;
    if (n >= (int)out_len) {
        out_buf[out_len - 1] = '\0';
        return (int)(out_len - 1);
    }
    return n;
}

// ------------------------- Scheduler core -------------------------

void AppScheduler_Init(void)
{
    memset(g_sched, 0, sizeof(g_sched));
    g_sched_last_poll_ms = 0;
    memset(&g_last_rtc_dt, 0, sizeof(g_last_rtc_dt));
    g_rtc_inited = 0;
}

static int sched_find_slot(const NanoJson_t *t)
{
    if (!t) return -1;

    // match by dev triple
    for (int i = 0; i < APP_SCHED_MAX_ITEMS; i++) {
        if (!g_sched[i].used) continue;
        if (strncmp(g_sched[i].mb_id, t->mb_id, sizeof(g_sched[i].mb_id)) == 0 &&
            strncmp(g_sched[i].bs_id, t->bs_id, sizeof(g_sched[i].bs_id)) == 0 &&
            strncmp(g_sched[i].sn_id, t->sn_id, sizeof(g_sched[i].sn_id)) == 0) {
            return i;
        }
    }

    // empty slot
    for (int i = 0; i < APP_SCHED_MAX_ITEMS; i++) {
        if (!g_sched[i].used) return i;
    }

    return -1;
}

static void sched_fill_ids(SchedItem_t *it, const NanoJson_t *t)
{
    copy_fixed(it->mb_id, sizeof(it->mb_id), t->mb_id);
    copy_fixed(it->bs_id, sizeof(it->bs_id), t->bs_id);
    copy_fixed(it->sn_id, sizeof(it->sn_id), t->sn_id);

    // host byte
    uint8_t mb = 0, bs = 0, sn = 0;
    (void)parse_u8_dec(t->mb_id, &mb);
    (void)parse_u8_dec(t->bs_id, &bs);
    (void)parse_u8_dec(t->sn_id, &sn);

    it->mb_host = mb;
#if APP_NODE_IP_FROM_BS_ID
    it->node_host = bs;
#else
    it->node_host = sn;
#endif
}

static void sched_send_action(const SchedItem_t *it, const char *action)
{
    if (!it || !action) return;

    NanoJson_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    copy_fixed(tmp.mb_id, sizeof(tmp.mb_id), it->mb_id);
    copy_fixed(tmp.bs_id, sizeof(tmp.bs_id), it->bs_id);
    copy_fixed(tmp.sn_id, sizeof(tmp.sn_id), it->sn_id);

    char pkt[128];
    int n = Build_Action_Json(&tmp, action, pkt, sizeof(pkt));
    if (n <= 0) return;

    // 优先发节点(10.1.4.x)，否则发主板(10.1.3.x)
    if (it->node_host >= 1 && it->node_host <= 254) {
        UDP_SendTo_NodeIP(it->node_host, pkt, (uint32_t)n);
    } else if (it->mb_host >= 1 && it->mb_host <= 254) {
        UDP_SendTo_DynamicIP(it->mb_host, pkt, (uint32_t)n);
    }
}

static void sched_apply_daily(SchedItem_t *it, const NanoJson_t *t)
{
    uint16_t onm = 0, offm = 0;
    if (parse_hhmm_to_minute(t->on, &onm) != 0) return;
    if (parse_hhmm_to_minute(t->off, &offm) != 0) return;

    it->mode = NANO_MODE_DAILY;
    it->on_minute = onm;
    it->off_minute = offm;

    // reset triggers
    it->last_on_day = -1;
    it->last_off_day = -1;
    it->temp_started = 0;
    it->temp_finished = 0;
    it->start_ts = 0;
    it->end_ts = 0;
}

static void sched_apply_temp(SchedItem_t *it, const NanoJson_t *t)
{
    // 必须有 RTC 当前时间，才能计算 start/end 的日期
    DateTime_t nowdt;
    if (rtc_get_datetime(&nowdt) != 0) {
        // 没读到 RTC 就先不设置（等待下一次）
        return;
    }

    uint16_t startm = 0;
    if (parse_hhmm_to_minute(t->start, &startm) != 0) return;

    uint32_t now_ts = dt_to_seconds(&nowdt);

    // today @ start
    DateTime_t startdt = nowdt;
    startdt.sec = 0;
    startdt.hour = (uint8_t)(startm / 60);
    startdt.min  = (uint8_t)(startm % 60);

    uint32_t start_ts = dt_to_seconds(&startdt);

    // 若 start 已经过了，则默认安排到下一天
    if ((int32_t)(start_ts - now_ts) < 0) {
        start_ts += 86400;
    }

    uint32_t end_ts = start_ts + (uint32_t)t->dur_min * 60;

    it->mode = NANO_MODE_TEMP;
    it->start_ts = start_ts;
    it->end_ts = end_ts;
    it->temp_started = 0;
    it->temp_finished = 0;

    // daily fields not used
    it->on_minute = 0;
    it->off_minute = 0;
    it->last_on_day = -1;
    it->last_off_day = -1;
}

int AppScheduler_HandleJson(const char *json)
{
    NanoJson_t t;
    if (Parse_NanoPi_Json(json, &t) != 0) return -1;

    // 校时
    if (t.time_str[0]) {
        DateTime_t dt;
        if (parse_datetime(t.time_str, &dt) == 0) {
            rtc_set_datetime(&dt);
        }
    }

    int idx = sched_find_slot(&t);
    if (idx < 0) return -1;

    SchedItem_t *it = &g_sched[idx];
    memset(it, 0, sizeof(*it));
    it->used = 1;
    sched_fill_ids(it, &t);

    if (t.mode == NANO_MODE_DAILY) {
        sched_apply_daily(it, &t);
    } else if (t.mode == NANO_MODE_TEMP) {
        sched_apply_temp(it, &t);
    } else {
        return -1;
    }

    return 0;
}

void AppScheduler_Tick(uint32_t now_ms)
{
    // 每 1s 读一次 RTC
    if ((uint32_t)(now_ms - g_sched_last_poll_ms) < 1000) return;
    g_sched_last_poll_ms = now_ms;

    DateTime_t nowdt;
    if (rtc_get_datetime(&nowdt) != 0) {
        // 若 RTC 读失败，就不触发
        return;
    }

    uint32_t now_ts = dt_to_seconds(&nowdt);
    int32_t day = date_to_days(nowdt.year, nowdt.month, nowdt.day);
    uint16_t now_minute = (uint16_t)(nowdt.hour * 60 + nowdt.min);

    for (int i = 0; i < APP_SCHED_MAX_ITEMS; i++)
    {
        SchedItem_t *it = &g_sched[i];
        if (!it->used) continue;

        if (it->mode == NANO_MODE_DAILY)
        {
            if (now_minute == it->on_minute && it->last_on_day != day) {
                sched_send_action(it, "on");
                it->last_on_day = day;
            }
            if (now_minute == it->off_minute && it->last_off_day != day) {
                sched_send_action(it, "off");
                it->last_off_day = day;
            }
        }
        else if (it->mode == NANO_MODE_TEMP)
        {
            if (!it->temp_started && (int32_t)(now_ts - it->start_ts) >= 0) {
                sched_send_action(it, "on");
                it->temp_started = 1;
            }
            if (it->temp_started && !it->temp_finished && (int32_t)(now_ts - it->end_ts) >= 0) {
                sched_send_action(it, "off");
                it->temp_finished = 1;
                it->used = 0; // temp 单次执行完就释放
            }
        }
    }
}

int ds1307_pro(void)
{
    uint32_t to = 200000;

    // Try bus recovery if lines are stuck low (START would never complete)
    i2c2_bus_recover_if_needed();

    // 等 BUSY 清掉
    while ((EXT_RTC_I2C->STAR2 & I2C_STAR2_BUSY) && to--) {}
    if (!to) return -1;

    I2C_GenerateSTART(EXT_RTC_I2C, ENABLE);
    if (i2c_wait_event(I2C_EVENT_MASTER_MODE_SELECT, 200000) != 0) goto err;

    // WCH SPL expects 7-bit address here
    I2C_Send7bitAddress(EXT_RTC_I2C, EXT_RTC_ADDR_7BIT, I2C_Direction_Transmitter);
    if (i2c_wait_event(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED, 200000) != 0) goto err;

    I2C_GenerateSTOP(EXT_RTC_I2C, ENABLE);
    return 0;

err:
    i2c_abort_recover();
    return -1;
}

