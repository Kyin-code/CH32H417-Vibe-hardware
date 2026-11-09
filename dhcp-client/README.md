# dhcp-client — 可移植的 DHCPv4 客户端（CH32H417 / WCHNET + host 测试）

从零实现的 DHCP 客户端，**不复用厂商内置 DHCP**（`WCHNET_DHCPStart` 那套）。
协议层与状态机与硬件无关：在 PC 上能跑完整测试，在 CH32H417 上通过一层适配器接入 WCH NetLib/ETH。

- 纯 C99，无动态内存、无浮点、无 `printf` 依赖（日志走可选回调）。
- host 测试：**2118 + 123 + 128 项断言全部通过**（CMake + Ninja + CTest，5 个测试）。
- CH32H417 适配层用厂商公开 API（`wchnet.h`）实现，并用**仓库里真实的 `wchnet.h`** 做主机端类型检查。

## 0. 验证等级（重要，别混淆）

| 等级 | 做了什么 | 结果 |
|---|---|---|
| **A. host 单元测试** | 2118 项断言覆盖报文编解码、状态机、租约、重启恢复、畸形输入 | ✅ 全绿 |
| **B. 适配层实际执行** | CH32H417 适配层 + 真实 `wchnet.h` + 假 WCHNET 栈，在主机上真跑（UDP / IP_RAW 两种传输） | ✅ 251 项断言全绿 |
| **C. 主机端类型检查** | `dhcp_port_wchnet.c` 用真实 `wchnet.h` + `net_config.h` 做 `-fsyntax-only`，两种模式，严格警告 | ✅ 零警告 |
| **D. 目标编译** | WCH RISC-V 工具链 / MounRiver 未安装 | ❌ **未做**（见第 13 节） |
| **E. 真机运行** | 需要板子 + 真实 DHCP 服务器 | ❌ **未做**（清单见第 12 节） |

核心逻辑（A）和适配层逻辑（B）已被测试证明；**没被证明的是 libwchnet.a 的实际行为**（D/E）。
B 用的是我写的假栈，它能验证“适配层对 WCHNET API 的用法正确”，但不能代替真实固件。

---

## 1. 目录结构

```
dhcp-client/
├── include/
│   ├── dhcp_config.h      编译期参数 + IPv4/时间小工具
│   ├── dhcp_packet.h      报文构建/解析 API（BOOTP + DHCP option）
│   ├── dhcp_port.h        平台适配层契约（时间/随机/发送/日志/租约存储）
│   └── dhcp_client.h      公共 API：状态机、租约、事件、统计
├── src/
│   ├── dhcp_packet.c      序列化 + 带边界检查的解析
│   ├── dhcp_client.c      状态机、定时器、重传退避、ACK 认证、租约计算
│   └── dhcp_lease.c       租约持久化 blob（magic + version + CRC32）
├── examples/
│   ├── lease_apply_policy.h/.c  纯逻辑：租约是否需要重新编程网络栈（host 可测）
│   └── v5f_bringup.c            CH32H417 V5F 集成示例（目标侧，未编译）
├── ports/
│   ├── host/              虚拟时钟、确定性 PRNG、TX 捕获、NVM 模拟
│   └── ch32h417/          WCHNET 适配层（UDP socket / IP_RAW 双模式）
│       ├── sim/           假 WCHNET 栈：让适配层在主机上真跑（非交付物）
│       └── _selfcheck/    厂商头文件缺失时使用的语法检查桩（不进目标工程）
├── tests/                 test_packet / test_client / test_policy + 测试框架
├── CMakeLists.txt         host 构建：库 + 测试 + 适配层类型检查
└── README.md
```

## 2. 分层

```
                应用（主循环 / RTOS task）
                          │  dhcp_client_poll()   dhcp_client_input()
                          ▼
   ┌──────────────────────────────────────────────────┐
   │ dhcp_client.c   状态机 / 定时器 / ACK 认证 / 租约 │
   │ dhcp_lease.c    租约 blob（CRC32）                │
   ├──────────────────────────────────────────────────┤
   │ dhcp_packet.c   BOOTP 头 + option 编解码（无 I/O）│
   └──────────────────────────────────────────────────┘
                          │ dhcp_port_t（函数指针表）
        ┌─────────────────┼──────────────────┐
        ▼                 ▼                  ▼
   now_ms/rand32    tx_udp(payload)     lease_save/load/clear
        │                 │                  │
   平台定时器        WCHNET socket      .noinit / flash / FRAM
```

## 3. 状态机

```
                    dhcp_client_start()
   INIT ─────────────────────────────► SELECTING ──OFFER──► REQUESTING
     ▲                                  │    ▲                 │
     │                        no offer  │    │ request timeout  │ ACK
     │                        (retries) │    └─────────────────┘
     │                                  ▼                      ▼
     │                              FAILED────────────────► BOUND
     │                                                       │ T1
     │                        lease expired                  ▼
     │             ┌────────────────────────────────── RENEWING
     │             │                                        │ T2
     │             │                                        ▼
     └─────────────┴─────────────────────────────  REBINDING
                                                   │ lease expired
   BOUND/RENEWING/REBINDING ── dhcp_client_release() ──► RELEASING ──► STOPPED
   NAK ──► INIT（等 nak_wait_ms 后重新 DISCOVER）
   dhcp_client_decline() ──► 发 DHCPDECLINE ──► INIT（等 decline_wait_ms）
   dhcp_client_restore() ──► INIT-REBOOT（用 option 50 校验旧租约）
```

| 状态 | 进入条件 | 发出 | 离开 |
|---|---|---|---|
| `INIT` | 初始/失败/NAK 后 | 无 | `start()` 或 hold-off 到时 |
| `SELECTING` | 开始获取 | `DHCPDISCOVER`（广播，新 xid） | OFFER 收集窗口结束 / 重传耗尽 |
| `REQUESTING` | 收集窗口结束 | `DHCPREQUEST`（广播，option 50+54） | ACK→BOUND / NAK→INIT / 超时→SELECTING |
| `BOUND` | ACK | 无 | `T1`→RENEWING / 到期→重新 DISCOVER |
| `RENEWING` | `T1` | `DHCPREQUEST`（单播，ciaddr，无 50/54） | ACK→BOUND / `T2`→REBINDING |
| `REBINDING` | `T2` | `DHCPREQUEST`（广播，ciaddr） | ACK→BOUND / 到期→丢弃地址 |
| `RELEASING` | `release()` | `DHCPRELEASE`（单播到 server） | 立即 STOPPED |
| `INIT_REBOOT` | `restore()` | `DHCPREQUEST`（广播，option 50，无 54） | ACK→BOUND / NAK→INIT / 超时→SELECTING |
| `STOPPED` | `stop()`/`release()` | 无 | `start()` |

**事件时序**：`set_state()` 先更新 `cl->state`，再以 `old_state = 前一个状态` 触发
`DHCP_EVENT_STATE_CHANGE`，所以回调里恒有 `old != new` 且 `cl->state == new`。
其它事件（BOUND/RENEWED/NAK/…）在状态已经切换后触发，`old_state` 传事件发生时的状态。

## 4. 关键行为

### 4.1 xid 与重传
- xid = `rand32() ^ 单调时钟*黄金比例 ^ MAC 位`；DISCOVER/REQUESTING 用同一个 xid，进入续租时换新。
- 回复必须 `op == BOOTREPLY`、xid 匹配、chaddr 前 6 字节等于本机 MAC，否则丢弃并计 `rx_ignored`。
- 重传：`retry_base_ms` 起，每次翻倍封顶 `retry_max_ms`，叠加 ±12.5% 抖动；每状态 `max_retries` 上限。
  超过后：SELECTING→FAILED，REQUESTING→重新 SELECTING，INIT_REBOOT→SELECTING。

### 4.2 ACK 认证（严格按状态区分）
| 状态 | 地址校验 | option 54 要求 |
|---|---|---|
| REQUESTING | `yiaddr == candidate.address` | **必须存在**，且等于我们选中的 server-id；缺失即丢弃 |
| RENEWING | `yiaddr == lease.address` | **必须存在**，且等于当前 server-id；缺失或不等即丢弃 |
| REBINDING | `yiaddr == lease.address` | 可缺失；存在则必须是合法非 0 值，并**采用为新 server**（RFC 2131 4.3.2） |
| INIT-REBOOT | `yiaddr == lease.address` | 可缺失；存在则必须是合法非 0 值，保留原 server |

另外：`yiaddr == 0` 视为畸形；`lease_time == 0` 视为畸形；ACK 的地址与我们请求的不一致一律丢弃。

### 4.3 租约时间
- 服务器给 option 51/58/59 就校验并使用：必须 `0 < T1 < T2 < lease`，否则回退 50% / 87.5%。
- `lease = 0xFFFFFFFF` 视为无限：不设任何定时器，`next_timeout` 返回 `DHCP_POLL_INFINITE`。
- **最小租约有策略**：没有下限（任何 `> 0` 的租约都接受，短租约自然得到短的 T1/T2）；
  `lease_time == 0` 判为畸形；**缺少 option 51 时回退为 1 小时**，避免出现无界租约。
- 毫秒时间戳会 49.7 天回绕，所有比较用回绕安全的差值；超长租约按 `0x7FFFFF00 ms` 截顶。

### 4.4 调度器 deadline
`dhcp_client_next_timeout_ms()` 返回**当前状态下所有相关 deadline 的最小值**：
- `BOUND` → `T1`；`RENEWING` → `min(重传 deadline, T2, 到期)`；`REBINDING` → `min(重传, 到期)`；
- `REQUESTING`/`INIT_REBOOT` → `min(重传, 到期)`；`SELECTING` → `min(OFFER 窗口, 重传)`；`INIT` → hold-off。

RENEWING 把 **T2 纳入**是关键：当重传间隔配置得比“距 T2 的剩余时间”更长时，若不考虑 T2 就会睡过头、错过 REBINDING。

### 4.5 option 边界检查
以 `len` 为唯一权威；PAD/END 正确跳过；option 声明长度超出剩余字节 → `DHCP_PARSE_ERR_OPTIONS`；
定长 option 长度不符 → 忽略该 option（不崩溃、不填充）；DNS/Router 列表长度必须是 4 的倍数，超出容量丢弃；
未知 option 跳过；`sname`/`file` 里的 option overload（52）不解析（见限制）。

### 4.6 租约持久化
56 字节 blob：magic `"DHCP"` + version + flags + 字段 + CRC32。
`dhcp_lease_deserialize()` 拒绝短包、错 magic、版本不符、CRC 错、地址为 0。

## 5. 公共 API

```c
#include "dhcp_client.h"

static dhcp_client_t  g_cl;
static dhcp_port_t    g_port;   /* 由平台填充 */

static void on_event(void *user, dhcp_event_t ev, dhcp_state_t old, const dhcp_client_t *cl)
{
    if (ev == DHCP_EVENT_BOUND || ev == DHCP_EVENT_RENEWED) {
        const dhcp_lease_t *l = dhcp_client_get_lease(cl);
        /* l->address / subnet_mask / router / dns[] … */
    }
}

void net_init(void)
{
    dhcp_client_cfg_t cfg;
    dhcp_client_cfg_default(&cfg);
    memcpy(cfg.mac, my_mac, 6);
    strcpy(cfg.hostname, "node-1");        /* 空串则不发送 option 12 */

    dhcp_client_init(&g_cl, &cfg, &g_port, on_event, 0);
    dhcp_client_start(&g_cl);
}

void net_task(void)
{
    dhcp_client_poll(&g_cl);               /* 定时器/重传/T1/T2/到期 */
    /* 收到 UDP/68 数据：dhcp_client_input(&g_cl, payload, len); */
}
```

| 函数 | 说明 |
|---|---|
| `dhcp_client_cfg_default()` | 默认配置 |
| `dhcp_client_init()` | 绑定 cfg + port + 回调，进入 INIT |
| `dhcp_client_start()` | 开始/重启获取，立即发 DISCOVER |
| `dhcp_client_restore()` | 用持久化租约走 INIT-REBOOT |
| `dhcp_client_poll()` | 驱动所有定时器 |
| `dhcp_client_input()` | 送入一个 DHCP 载荷：1=接受 / 0=忽略 / <0=畸形 |
| `dhcp_client_release()` | 发 RELEASE 并停止，清持久化租约 |
| `dhcp_client_decline()` | 应用检测到地址冲突时发 DECLINE |
| `dhcp_client_stop()` | 静默停止 |
| `dhcp_client_next_timeout_ms()` | 距下次 poll 的毫秒数 |
| `dhcp_lease_serialize/deserialize()` | 租约 blob 编解码 |

统计：`tx_discover/tx_request/tx_decline/tx_release/rx_offer/rx_ack/rx_nak/rx_ignored/rx_malformed/retransmits/tx_errors`。

## 6. 平台适配层

```c
typedef struct dhcp_port {
    void *ctx;
    uint32_t (*now_ms)(void *ctx);                                        /* 必需 */
    uint32_t (*rand32)(void *ctx);                                        /* 必需 */
    int      (*tx_udp)(void *ctx, const uint8_t *data, uint16_t len,      /* 必需 */
                       uint32_t dst_ip, uint16_t dst_port);
    void     (*log)(void *ctx, dhcp_log_level_t, const char *msg);        /* 可选 */
    int      (*lease_save)(void *, const uint8_t *blob, uint16_t len);    /* 可选 */
    int      (*lease_load)(void *, uint8_t *blob, uint16_t cap, uint16_t *len);
    int      (*lease_clear)(void *);
} dhcp_port_t;
```

## 7. CH32H417 / WCHNET 集成（V5F）

### 7.1 已核实的厂商事实

读 `doc/EVT/EXAM/ETH/DHCP`、`NetLib/wchnet.h`、`eth_driver.h`、`net_config.h`，以及 `nm -g libwchnet.a`：

- 栈是 **lwIP 派生**（`libwchnet.a` 含 `dhcp_start/udp_input/etharp_output/WCHNetIf` 等符号），但 EVT 树**不提供 lwip 头文件**，只能用 `wchnet.h` 公开 API。
- 轮询模型：主循环 `WCHNET_MainTask()` + `if (WCHNET_QueryGlobalInt()) WCHNET_HandleGlobalInt();`，1 ms 时基由 TIM2 中断里的 `WCHNET_TimeIsr()` 驱动。
- 收数据靠 `WCHNET_SocketRecvLen()` 查询、`WCHNET_SocketRecv()` 取走；`SINT_STAT_RECV` 只是“有数据”标志。
- socket 中断位需要用 `WCHNET_GetSocketInt(id)` **读并清除**（官方示例在 `WCHNET_HandleGlobalInt()` 里遍历）。
- `SOCK_INF.ProtoType = PROTO_TYPE_UDP` 时 `SourPort/DesPort` 是端口；`PROTO_TYPE_IP_RAW` 时 `SourPort` 是 IP 协议号，且调用方只提供 L4 载荷（见 `IPRaw_PING`）。
- `WCHNET_SocketUdpSendTo()` 可按报文指定目标 IP/端口 → RENEWING 单播可用。
- 官方 DHCP 例程走 `WCHNET_DHCPStart(cb)`；本实现**替换**该路径，不要同时启用。

### 7.2 集成步骤

1. 把 `include/*.h`、`src/*.c`、`ports/ch32h417/*.c|*.h`、`examples/lease_apply_policy.*` 拷进工程。
2. MounRiver 里加入目标构建；include 路径加 `include/`、`ports/ch32h417/`、`examples/`。
3. **不要**调用 `WCHNET_DHCPStart/WCHNET_DHCPStop/WCHNET_DHCPSetHostname`。
4. `net_config.h`：UDP 模式要 `WCHNET_NUM_UDP >= 1`；IP_RAW 模式要 `WCHNET_NUM_IPRAW >= 1`；`WCHNET_MAX_SOCKET_NUM` 覆盖你所有 socket。
5. TIM2 ISR：`WCHNET_TimeIsr(WCHNETTIMERPERIOD);` 旁边加 `dhcp_wchnet_tick(&s_dhcp, WCHNETTIMERPERIOD);`
6. 主循环：`dhcp_wchnet_poll(&s_dhcp); dhcp_client_poll(&s_client);`
7. 栈初始化：首次 `ETH_LibInit(0,0,0,0,mac)`；DHCP 在未配置接口上跑（源 0.0.0.0、广播标志置位）。
8. 实现 `dhcp_wchnet_apply_lease()`（弱符号）把租约写进栈 —— 见 7.3。
9. 可选：用芯片 RNG 给 `ctx.rng` 播种。

### 7.3 把租约应用到栈上（重启策略已修正）

`libwchnet.a` 没有公开的“改本机 IP”函数，只有 `WCHNET_Init(ip,gw,mask,mac)`。
导出符号里确实有 lwIP 的 `WCHNetIf`、`netif_set_addr`、`netif_set_ipaddr/gw/netmask`、`netif_set_up`，但缺少匹配的 `lwip/netif.h`。

- **方案 A（示例默认）**：租约存 `.noinit`，仅在**地址/掩码/网关发生变化或尚未应用**时重启一次；重启后读回 blob 用租约地址 `ETH_LibInit()`，再 `dhcp_client_restore()` 走 INIT-REBOOT。
- **方案 B（需要匹配的 lwIP 头）**：`netif_set_addr(&WCHNetIf, …); netif_set_up(&WCHNetIf);`，热切换不重启。

**重启环已修复**：判定逻辑抽到 `examples/lease_apply_policy.c`（`lease_apply_decide/commit/reset`，纯逻辑、host 可测）。
`v5f_bringup.c` 只在 `decide() != LEASE_APPLY_NONE` 时才 commit 并置重启标志；`.noinit` 里同时保存“已应用的配置”，
所以 INIT-REBOOT 的 ACK（也会报 `DHCP_EVENT_RENEWED`）不会再触发第二次重启。
`tests/test_policy.c` 用 20 轮迭代证明：稳态租约只会重启 1 次，地址真的变化时才再 +1 次。

| 场景 | decide() | 动作 |
|---|---|---|
| 栈未配置 | `INITIAL` | commit + 重启一次 |
| 同一地址/掩码/网关 | `NONE` | 什么都不做 |
| 地址/掩码/网关变化 | `CHANGED` | commit + 重启一次 |
| 只有 DNS 变化 | `NONE` | 不重配栈 |
| 租约无效或地址为 0 | `NONE` | 什么都不做 |

### 7.4 传输选择

| 宏 | 传输 | 适用 |
|---|---|---|
| `DHCP_WCHNET_USE_UDP_SOCKET=1`（默认） | UDP socket + `WCHNET_SocketUdpSendTo` | 推荐：栈构造 IP/UDP 头，单播/广播都支持 |
| `DHCP_WCHNET_USE_UDP_SOCKET=0` | IP_RAW + 自建 UDP 头（校验和置 0） | UDP 广播发不出去时的后备；目标 IP 变化时重建 socket |

接收侧兼容两种：载荷开头若是合法 68→67 UDP 头则跳过 8 字节再交给解析器。

### 7.5 适配层的两个实现细节

- **时间基准**：`dhcp_wchnet_ctx_t.now_ms` 是 `volatile uint32_t`，只由 `dhcp_wchnet_tick()`（定时器 ISR）写、
  主循环读。两侧都是对齐 32 位访问（在 CH32H417 上原子），ISR 只做 `+=`、主循环只读，所以最多读到“差一个 tick”的值，不会撕裂。
  若将来有第二个写者（例如 V3F 核），必须加临界区——头文件里已注明扩展点。
- **socket 中断**：`dhcp_wchnet_poll()` 开头调用 `WCHNET_GetSocketInt(id)` 读并清除该 socket 的中断位，
  避免 `GINT_STAT_SOCKET` 残留导致反复进中断。若应用自己的 `WCHNET_HandleGlobalInt()` 已消费，返回 0 也无妨——
  接收路径由 `WCHNET_SocketRecvLen()` 驱动，不以 intstat 为条件。

### 7.6 最大的集成陷阱：厂商 `hardware.c` 会吞掉 DHCP 报文

`doc/EVT/EXAM/ETH/DHCP/Common/hardware.c` 里的 `WCHNET_HandleGlobalInt()` 不是库函数，而是**应用层代码**，它这样处理 socket 事件：

```c
if (intstat & GINT_STAT_SOCKET) {
    for (i = 0; i < WCHNET_MAX_SOCKET_NUM; i++) {
        socketint = WCHNET_GetSocketInt(i);
        if (socketint) WCHNET_HandleSockInt(i, socketint);   /* RECV -> DataLoopback() */
    }
}
```

而 `WCHNET_HandleSockInt()` 对 `SINT_STAT_RECV` 调 `WCHNET_DataLoopback(socketid)`，即**把收到的数据原样回环发出去并清空缓冲**。
把 DHCP 客户端接到任何基于该示例的工程时，DHCPOFFER/DHCPACK 会被这个回环吃掉：不报错、不崩溃，只是永远 bind 不上。

必须让那个循环跳过 DHCP 的 socket（port 提供了 `dhcp_wchnet_owns_socket()`）：

```c
if (intstat & GINT_STAT_SOCKET) {
    for (i = 0; i < WCHNET_MAX_SOCKET_NUM; i++) {
        if (dhcp_wchnet_owns_socket(&s_dhcp, (uint8_t)i)) continue;   /* <-- 新增 */
        socketint = WCHNET_GetSocketInt(i);
        if (socketint) WCHNET_HandleSockInt(i, socketint);
    }
}
```

并且在主循环里把 `dhcp_wchnet_poll()` 放在 `WCHNET_HandleGlobalInt()` 之前。`examples/v5f_bringup.c` 的主循环已按此写好并附注释。

## 8. 构建与测试

### host（本机验证过）

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure    # 5/5 passed
./build/dhcp_tests.exe                        # 2118 checks, 0 failure(s)
```

CMake 里的五个测试：

| 测试 | 内容 |
|---|---|
| `dhcp_unit_tests` | 2118 项断言的单元测试可执行文件 |
| `wchnet_port_udp_1` | CH32 适配层 + 假 WCHNET 栈，**真跑**一次完整握手（UDP socket 传输） |
| `wchnet_port_udp_0` | 同上（IP_RAW 传输） |
| `wchnet_selfcheck_udp_1` | 用真实 `wchnet.h` + `net_config.h` 对适配层做 `-fsyntax-only`（UDP socket） |
| `wchnet_selfcheck_udp_0` | 同上（IP_RAW） |

selfcheck 会自动查找 `../nanoCH32H417-main/doc/EVT/EXAM/ETH/NetLib/wchnet.h`；找不到时退回 `ports/ch32h417/_selfcheck/wchnet.h` 桩
（配置时会打印 `WCHNET selfcheck uses: vendor header (...)` 或 `stub header (...)`）。

不用 CMake 的等价命令：

```bash
gcc -std=c99 -Wall -Wextra -Werror -pedantic -Iinclude -Iports/host -Itests -Iexamples \
    src/*.c ports/host/*.c examples/lease_apply_policy.c tests/*.c -o dhcp_tests
```

### 严格警告（手工跑过，10 个文件全部零警告）

```bash
gcc -std=c99 -Wall -Wextra -Werror -pedantic -Wshadow -Wconversion \
    -Wsign-conversion -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes \
    -fstack-protector-strong -Iinclude -Iports/host -Itests -Iexamples -c <每个 .c>
```

适配层用真实厂商头文件：

```bash
gcc -fsyntax-only -std=c99 -Wall -Wextra -Werror -pedantic -Wshadow \
    -Wconversion -Wsign-conversion -Wcast-qual -Wstrict-prototypes \
    -DDHCP_WCHNET_USE_UDP_SOCKET=1 \
    -Iinclude \
    -I../nanoCH32H417-main/doc/EVT/EXAM/ETH/NetLib \
    -I../nanoCH32H417-main/doc/EVT/EXAM/ETH/DHCP/Common \
    ports/ch32h417/dhcp_port_wchnet.c
# 把 =1 换成 =0 再跑一次（IP_RAW 模式）
```

> `WCHNET_SocketUdpSendTo()` 的参数未标 `const`，适配层用 `union {const uint8_t *cp; uint8_t *p;}` 转交，
> 这样既不改厂商头文件的签名，也不会在 `-Wcast-qual` 下报警。

### Sanitizer

```bash
cmake -S . -B build-asan -G Ninja -DDHCP_SANITIZE=ON
```

本机 MinGW GCC 15.2 **缺少 `libasan`/`libubsan`**，链接会失败（环境限制，需 Linux/clang）。
替代手段：`-fstack-protector-strong`、逐字节截断循环、4000 次随机变异，以及下面的变异测试。

## 9. 测试覆盖（2118 断言）

`tests/test_packet.c` — 协议层
- DISCOVER/REQUEST 构建：BOOTP 字段、magic cookie、option 53 必须首、client-id、参数请求列表、按类型带 50/54、300 字节填充、容量不足/非法参数。
- ACK 解析：mask/router/DNS/lease/server-id/T1/T2 取值。
- 畸形：短于 240、空指针、非法 op、`hlen>16`、cookie 错、option 长度越界、缺 53、长度非 4 的 option 被忽略。
- 逐字节截断循环（0..len）；option 体中间截断必被检测；4000 次随机变异不崩溃。

`tests/test_client.c` — 状态机/租约
- 正常获取、T1/T2 默认与服务器自定义、非法 T1/T2 回退。
- 丢包重传与指数退避、重传后成功、REQUEST 超时回 SELECTING、超限 FAILED。
- NAK hold-off 后重新 DISCOVER。
- T1 单播续租（ciaddr 有值、无 50/54）、续租失败进 REBINDING、ACK 回 BOUND。
- 到期 → `LEASE_EXPIRED`、租约清零、不复用 NVM 里的旧值、重新 DISCOVER。
- 无限租约不续租且 `next_timeout = DHCP_POLL_INFINITE`。
- 恶意/异常：xid 不符、MAC 不符、BOOTREQUEST、畸形 OFFER、截断 OFFER、缺 server-id、ACK 地址不符、ACK 来自未选中服务器、`lease_time=0`。
- **严格 ACK 认证**：REQUESTING 缺 option 54 被拒；RENEWING 缺 54 或 server 不符被拒；REBINDING 接受新 server 并更新 server-id。
- **事件时序**：`STATE_CHANGE` 回调的 `old != new`、状态序列 `SELECTING→REQUESTING→BOUND`、BOUND 事件时状态已是 BOUND。
- **调度 deadline**：RENEWING 下 `next_timeout` 由 T2 限制（重传间隔故意配成 10 s > 距 T2 的 7 s）。
- 租约 blob 往返、单 bit 损坏被 CRC 拒、截断/错 magic 被拒。
- 重启恢复：INIT-REBOOT 带 50 无 54；ACK→BOUND（算续租）；NAK→丢弃回 DISCOVER。
- DECLINE / RELEASE / STOPPED / 重启 / TX 失败重试。

`tests/test_policy.c` — 示例的租约应用策略
- 首次 `INITIAL`、稳态 `NONE`、地址/掩码/网关变化 `CHANGED`、DNS-only 变化 `NONE`、无效租约 `NONE`。
- **重启环回归**：20 轮迭代只重启 1 次；地址真变时才 +1。

`tests/test_wchnet_port.c` — CH32 适配层**实际执行**（假 WCHNET 栈，UDP 与 IP_RAW 各一遍）
- `SOCK_INF` 字段：`ProtoType`/`SourPort`/`DesPort`/`IPAddr`/`RecvBufLen`/`RecvStartPoint` 都正确。
- DISCOVER 帧：UDP 模式交给 `SocketUdpSendTo(…, 255.255.255.255, 67)` 纯载荷；IP_RAW 模式自建的 8 字节 UDP 头（68→67、长度、校验和 0）逐字段核对。
- 接收：纯载荷 与 带 UDP 头 两种形式都能正确解析（验证 L4 剥离）；乱码/截断不崩溃。
- **超长报文**：>600 字节时整包丢弃、队列清空、不交给解析器。
- **部分发送**：驱动只写 10 字节时 `tx_errors` 增加、不计入 `tx_discover`，恢复后能重传成功。
- 完整握手 → BOUND 时 `apply_lease` 被调用一次且参数正确、租约已持久化。
- 续租：单播目标正确；IP_RAW 下因目标变化**重建 socket**（close+creat）。
- 重绑定：广播目标正确，且接受其他 server 的 ACK 并更新 server-id。
- `dhcp_wchnet_owns_socket()` 只认自己的 socket（供厂商 `hardware.c` 跳过）。
- 未 start 时 `poll` 是空操作。

## 10. 回归验证（变异测试）

为了确认这些测试真的能抓住回归，向 `src/dhcp_client.c` 注入三个旧 bug 并重跑测试，然后还原：

| 注入的变异 | 结果 |
|---|---|
| `next_timeout` 忽略 T2（`if (cl->t2_ms != 0u)` → `if (0)`） | 1 项失败（`nt <= 7000u`） |
| 关闭 ACK 的 server-id 校验（`require_server = 1` → `0`） | 12 项失败 |
| `STATE_CHANGE` 回调把 `next` 当 old 传出 | 4 项失败（`old_states[i] != states[i]`） |
| 移除适配层的部分发送检查 | 两个传输各 **3 项失败**（`tx_errors` 不增） |
| 移除超长报文丢弃（回到 clamp） | 两个传输均 **进程崩溃**（exit `0xC0000005`，600 字节栈缓冲溢出） |

还原后：`2118 checks, 0 failure(s)`，`ctest` 5/5。

## 11. 已知限制

1. **未在目标上编译**、**未在真机上运行**（第 0 节 C/D 级）。
2. **没有 ARP 冲突探测**：DECLINE 只能由应用触发（链路层 ARP probe 发现冲突后调用 `dhcp_client_decline()`）。
3. **不支持 option overload（52）**：`sname`/`file` 里的附加 option 不解析。
4. 不发送 DHCPINFORM；不支持 DHCPv6；不支持 RFC 3442 option 121 静态路由。
5. IP_RAW 模式的 UDP 校验和写 0（IPv4 允许；未配置源地址时也无法正确计算伪首部）。
6. 时间分辨率秒级；32 位毫秒会在 49.7 天回绕（已用回绕安全比较，超长租约按 `0x7FFFFF00 ms` 截顶）。
7. 方案 A 的一次性重启是“改地址”的代价；方案 B 需要匹配的 lwIP 头文件，未在本仓库验证。
8. 未做 RFC 一致性套件级别的验证（如 option 强制顺序、giaddr 中继场景）。
9. **与厂商示例的中断处理冲突**：不改 `hardware.c` 里 `WCHNET_HandleSockInt()` 的 `DataLoopback()` 调用，DHCP 报文会被回环吞掉（见 7.6）。这不是本实现的 bug，但是必改项。
10. **状态机一次 poll 只走一步**：若主循环长时间阻塞，一次 poll 跳过了 T1 与 T2，需要多一次 poll 才进入 REBINDING（毫秒级，实际影响很小，已在适配层测试中验证补 poll 后正确）。
11. 适配层的验证基于自写的假栈（`ports/ch32h417/sim/`），**不能代替** libwchnet.a 的真实行为。

## 12. 硬件验证清单（真机，尚未执行）

1. [ ] 工程能编译：`src/*.c` + `ports/ch32h417/dhcp_port_wchnet.c` + `examples/lease_apply_policy.c` 在 MounRiver 下零警告。
2. [ ] `ETH_LibInit(0,0,0,0,mac)` + `dhcp_wchnet_start()` 后抓包看到 DISCOVER（0.0.0.0 → 255.255.255.255:67，chaddr=本机 MAC，broadcast flag 置位）。
3. [ ] OFFER 后看到 REQUEST（option 50 = offer 地址，option 54 = server-id）。
4. [ ] ACK 后 `dhcp_wchnet_apply_lease()` 被调用一次，地址/掩码/网关/DNS 与服务器一致。
5. [ ] **只重启一次**：第一次 BOUND 重启，之后 INIT-REBOOT ACK 不再重启（串口日志里只应出现一次 `DHCP applied`）。
6. [ ] 租约改成 60 s：T1（30 s）观察单播 REQUEST 并续租成功，地址不变。
7. [ ] 续租期间断开服务器：T2（52 s）观察广播 REQUEST；到期后 `LEASE_EXPIRED` 并重新 DISCOVER。
8. [ ] 服务器换池：NAK 被识别，hold-off 后拿到新地址，且此时重启恰好一次。
9. [ ] 服务器强制 NAK：客户端不保留旧地址。
10. [ ] ≥24 h 运行：无泄漏、`stats.retransmits` 合理、无 xid 误判。
11. [ ] 同时跑厂商 TCP/UDP socket：`WCHNET_MAX_SOCKET_NUM` 够用、socket id 不冲突。
12. [ ] socket 中断不残留：长时间不轮询时 `GINT_STAT_SOCKET` 不会一直置位（验证 `WCHNET_GetSocketInt` 调用点）。
13. [ ] IP_RAW 后备传输（`-DDHCP_WCHNET_USE_UDP_SOCKET=0`）：广播 DISCOVER 与单播 RENEW 都能发出。

## 13. 环境与阻塞

| 项 | 状态 |
|---|---|
| host gcc | ✅ `gcc 15.2.0` (MinGW-w64)；`-Wall -Wextra -Werror -pedantic -Wshadow -Wconversion -Wsign-conversion -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes` 零警告 |
| host 构建/测试 | ✅ CMake 3.28.1 + Ninja，`ctest` 5/5：单元 2118 项 + 适配层 123/128 项断言全绿 |
| 适配层类型检查 | ✅ 真实 `wchnet.h` + `net_config.h`，UDP/IP_RAW 两模式零警告（已接入 CTest） |
| 适配层实际执行 | ✅ 假 WCHNET 栈，UDP 123 项 + IP_RAW 128 项断言全绿（**仍不等于 libwchnet.a 行为**） |
| host sanitizer | ⚠️ MinGW 缺 `libasan`/`libubsan`（Linux/clang 可用） |
| **WCH RISC-V 工具链** | ❌ **未安装**（`C:\WCH.CN` 仅串口驱动，PATH 无 `riscv-wch-elf-gcc`，无 MounRiver）→ **目标工程无法编译** |
| **真机** | ❌ **未执行**，见第 12 节清单 |

## 14. 参考资料

- `nanoCH32H417-main/doc/EVT/EXAM/ETH/DHCP/`（官方 DHCP 例程，使用 `WCHNET_DHCPStart`）
- `nanoCH32H417-main/doc/EVT/EXAM/ETH/NetLib/wchnet.h`（socket API）
- `nanoCH32H417-main/doc/EVT/EXAM/ETH/NetLib/eth_driver.h`（`ETH_LibInit` 等）
- `nanoCH32H417-main/doc/EVT/EXAM/ETH/DHCP/Common/net_config.h`（socket 池 / 定时器周期）
- `nanoCH32H417-main/doc/EVT/EXAM/ETH/IPRaw_PING/`（IP_RAW 传输语义）
- RFC 2131（DHCP）、RFC 2132（option）、RFC 951/1542（BOOTP/中继最小报文）
