#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "tusb.h"
#include "pico/multicore.h"
#include "hardware/timer.h"

// =================配置区域=================
#define WIFI_SSID "你的WiFi名称"
#define WIFI_PASSWORD "你的WiFi密码"
#define UDP_PORT 8888

// 定义接口索引 (必须与 usb_descriptors.c 一致)
// 0号接口是键盘，1号接口是鼠标
#define ITF_NUM_KEYBOARD 0
#define ITF_NUM_MOUSE 1

// 定义报告 ID (必须与 usb_descriptors.c 一致)
#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE 2
// ==========================================

// 协议命令字
enum
{
    CMD_PRESS_KEY = 1,
    CMD_MOVE_DIRECT = 3,
    CMD_MOVE_HUMAN = 4,
    CMD_CLICK_BUTTON = 8
};

// 坐标点结构体
typedef struct
{
    float x;
    float y;
} Point;

// 辅助：非阻塞睡眠，保持USB心跳
static inline void sleep_ms_custom(uint32_t ms)
{
    if (ms == 0)
        return;
    absolute_time_t target = delayed_by_ms(get_absolute_time(), ms);
    while (absolute_time_diff_us(get_absolute_time(), target) > 0)
    {
        tud_task(); // 喂狗：保持USB连接活跃
    }
}

// 辅助：获取带随机噪声的延迟时间
// target_ms: 目标延迟, variance: 波动范围
uint32_t get_noisy_delay(uint32_t target_ms, uint32_t variance)
{
    if (target_ms == 0)
        return 0;
    int noise = (rand() % (variance * 2 + 1)) - variance; // 生成 -variance 到 +variance
    int result = target_ms + noise;
    return (result < 1) ? 1 : result;
}

// ---------------------------------------------------------
// 核心算法：三次贝塞尔曲线插值
// ---------------------------------------------------------
static Point cubic_bezier(Point p0, Point p1, Point p2, Point p3, float t)
{
    float u = 1.0f - t;
    float tt = t * t;
    float uu = u * u;
    float uuu = uu * u;
    float ttt = tt * t;

    Point p;
    p.x = uuu * p0.x + 3 * uu * t * p1.x + 3 * u * tt * p2.x + ttt * p3.x;
    p.y = uuu * p0.y + 3 * uu * t * p1.y + 3 * u * tt * p2.y + ttt * p3.y;
    return p;
}

// ---------------------------------------------------------
// 核心功能：拟人化移动 (非阻塞版)
// ---------------------------------------------------------
static void move_relative_human(int dx, int dy, float speed_pct, float curvature_pct)
{
    // 1. 计算物理距离
    float distance = sqrtf(dx * dx + dy * dy);
    if (distance < 1.0f)
        return;

    // 2. 根据距离动态计算步数 (步长越短越平滑，但也越慢)
    // 距离远时步子大一点，距离近时步子小一点
    int steps = (int)(distance / 2.0f);
    if (steps < 5)
        steps = 5; // 至少分5步走完
    if (steps > 200)
        steps = 200; // 限制最大步数防止超时

    // 3. 定义贝塞尔控制点
    Point p0 = {0, 0};                 // 起点 (相对坐标系原点)
    Point p3 = {(float)dx, (float)dy}; // 终点

    // 计算垂直于路径的法向量，用于施加曲率偏移
    float perp_x = -dy;
    float perp_y = dx;
    // 归一化法向量
    float len = sqrtf(perp_x * perp_x + perp_y * perp_y);
    if (len > 0)
    {
        perp_x /= len;
        perp_y /= len;
    }

    // 曲率偏移量：距离 * 百分比
    float offset = distance * (curvature_pct / 100.0f) * 0.5f;

    // 设置两个控制点，拉扯出弧线
    Point p1 = {dx / 3.0f + perp_x * offset, dy / 3.0f + perp_y * offset};
    Point p2 = {2.0f * dx / 3.0f + perp_x * offset, 2.0f * dy / 3.0f + perp_y * offset};

    // 4. 根据 speed_percent 计算基础延迟 (ms)
    // 100%速度 -> 2ms延迟, 0%速度 -> 15ms延迟
    uint32_t base_delay = 15 - (uint32_t)(13.0f * (speed_pct / 100.0f));
    if (base_delay < 2)
        base_delay = 2;

    Point last_pt = p0;
    float overflow_x = 0; // 累积浮点数误差
    float overflow_y = 0;

    for (int i = 1; i <= steps; i++)
    {
        float t = (float)i / steps;

        // 变速逻辑：使用 Ease-Out (快起步，慢结束) 或 Ease-In-Out
        // 这里使用简单的线性 t，但配合变化的 sleep 时间来实现变速

        Point current_pt = cubic_bezier(p0, p1, p2, p3, t);

        // 计算这一步的增量
        float delta_x_f = current_pt.x - last_pt.x + overflow_x;
        float delta_y_f = current_pt.y - last_pt.y + overflow_y;

        int8_t step_x = (int8_t)delta_x_f;
        int8_t step_y = (int8_t)delta_y_f;

        // 保存小数部分留给下一步 (防丢帧)
        overflow_x = delta_x_f - step_x;
        overflow_y = delta_y_f - step_y;

        // 仅当有移动时才发送 USB 包
        if (step_x != 0 || step_y != 0)
        {
            if (tud_hid_n_ready(ITF_NUM_MOUSE))
            {
                // 注意：这里使用的是 ITF_NUM_MOUSE (1)
                tud_hid_n_mouse_report(ITF_NUM_MOUSE, REPORT_ID_MOUSE, 0, step_x, step_y, 0, 0);
            }
        }

        last_pt = current_pt;

        // 5. 动态延迟与随机抖动
        // 在路径的中段速度快，两头慢 (Fitts定律模拟)
        uint32_t current_delay = base_delay;

        // 简单的两头减速算法
        if (t < 0.1f || t > 0.9f)
            current_delay += 3;

        // 加入随机噪声 (+/- 1ms)
        sleep_ms_custom(get_noisy_delay(current_delay, 1));
    }
}

// ---------------------------------------------------------
// UDP 回调处理
// ---------------------------------------------------------
static void udp_server_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    if (p != NULL)
    {
        // 确保数据包长度足够
        if (p->len >= 1)
        {
            uint8_t *data = (uint8_t *)p->payload;
            uint8_t cmd = data[0];

            switch (cmd)
            {
            case CMD_MOVE_HUMAN:
            {
                // 协议格式: [CMD(1)] [DX(2)] [DY(2)] [SPEED(4)] [CURVE(4)] = 13字节
                if (p->len >= 13)
                {
                    int16_t x, y;
                    float speed, curve;
                    memcpy(&x, &data[1], 2);
                    memcpy(&y, &data[3], 2);
                    memcpy(&speed, &data[5], 4);
                    memcpy(&curve, &data[9], 4);

                    move_relative_human(x, y, speed, curve);
                }
                break;
            }
            case CMD_PRESS_KEY:
            {
                // 协议格式: [CMD(1)] [KEYCODE(1)] [DELAY_MS(2)]
                if (p->len >= 4)
                {
                    uint8_t key = data[1];
                    uint16_t delay;
                    memcpy(&delay, &data[2], 2);

                    // 按下
                    if (tud_hid_n_ready(ITF_NUM_KEYBOARD))
                    {
                        uint8_t key_arr[6] = {key, 0, 0, 0, 0, 0};
                        tud_hid_n_keyboard_report(ITF_NUM_KEYBOARD, REPORT_ID_KEYBOARD, 0, key_arr);
                    }
                    tud_task();

                    // 保持 (带一点点随机波动)
                    sleep_ms_custom(get_noisy_delay(delay, 5));

                    // 抬起
                    if (tud_hid_n_ready(ITF_NUM_KEYBOARD))
                    {
                        tud_hid_n_keyboard_report(ITF_NUM_KEYBOARD, REPORT_ID_KEYBOARD, 0, NULL);
                    }
                    tud_task();
                }
                break;
            }
            case CMD_CLICK_BUTTON:
            {
                // 协议格式: [CMD(1)] [BTN_MASK(1)] (1=左键, 2=右键)
                if (p->len >= 2)
                {
                    uint8_t btn = data[1];
                    if (tud_hid_n_ready(ITF_NUM_MOUSE))
                    {
                        tud_hid_n_mouse_report(ITF_NUM_MOUSE, REPORT_ID_MOUSE, btn, 0, 0, 0, 0);
                    }
                    tud_task();
                    // 点击保持时间 60-100ms
                    sleep_ms_custom(get_noisy_delay(80, 20));

                    if (tud_hid_n_ready(ITF_NUM_MOUSE))
                    {
                        tud_hid_n_mouse_report(ITF_NUM_MOUSE, REPORT_ID_MOUSE, 0, 0, 0, 0, 0);
                    }
                    tud_task();
                }
                break;
            }
            }
        }
        pbuf_free(p);
    }
}

// ---------------------------------------------------------
// TinyUSB 必需回调
// ---------------------------------------------------------
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) { return 0; }
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {}

// ---------------------------------------------------------
// 主程序
// ---------------------------------------------------------
void core1_entry()
{
    while (true)
        cyw43_arch_poll(); // Core 1 专职网络轮询
}

int main()
{
    stdio_init_all();

    // 1. 初始化 WiFi 硬件
    if (cyw43_arch_init())
        return 1;
    cyw43_arch_enable_sta_mode();

    // 2. 连接 WiFi (阻塞直到连接成功)
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000))
    {
        return 1;
    }

    // 3. 性能优化：关闭 WiFi 节能模式 (大幅降低延迟)
    cyw43_wifi_pm(&cyw43_state, CYW43_NO_POWERSAVE_MODE);

    // 4. 重要：初始化随机种子 (防止每次重启轨迹一样)
    srand(time_us_32());

    // 5. 启动 Core 1 处理网络
    multicore_launch_core1(core1_entry);

    // 6. 初始化 USB
    tusb_init();

    // 7. 启动 UDP 服务
    struct udp_pcb *pcb = udp_new();
    udp_bind(pcb, IP_ADDR_ANY, UDP_PORT);
    udp_recv(pcb, udp_server_recv, NULL);

    // Core 0 主循环
    while (true)
    {
        tud_task(); // 处理 USB 事件
    }

    return 0;
}
