# CH32H417 目标固件构建（双核 Merge.Bin）

本目录把 `dhcp-client` 集成进 CH32H417 双核固件。**不修改任何厂商源码**：
自己实现的只有 `main_v3f.c`、`main_v5f.c`、`ch32h417_it.c` 和 `core_riscv.c`（后者是
`SRC/Core/core_riscv.c` 的副本，仅把新版 binutils 已移除的 WCH 私有 CSR `mucounteren`
按编号 `0x306` 写回）。

## 1. 双核启动模型

```
上电
 ├─ V3F (0x00000000, 64K 窗口)          main_v3f.c
 │    SystemInit() → 配时钟/串口 → NVIC_WakeUp_V5F(0x00010000) → PWR_EnterSTOPMode()
 │    之后 V3F 停在 STOP，不再参与
 └─ V5F (0x00010000, 128K 窗口)          main_v5f.c
      ETH_LibInit(0.0.0.0) → dhcp_wchnet_init → TIM2 1ms → 主循环
      TIM2 ISR: WCHNET_TimeIsr() + dhcp_wchnet_tick()
      主循环: dhcp_wchnet_poll → WCHNET_MainTask → QueryGlobalInt/GlobalInt(跳过 DHCP socket) → dhcp_client_poll
```

- V5F 与 V3F 的代码段 VMA 都在 RAM（`RAM_CODE` 0x200A0000 / 0x20100000），
  由各自 startup 的 `_load_base` 从 flash 拷到 RAM 执行；`Merge.bin` 里放的是 **LMA**（flash）。
- 两个核的 `Run_Core` 都设为 `Run_Core_V5F`（=1）：V3F 因此走“唤醒 V5F 并停车”分支，
  V5F 因此得到 `Func_Run_V5F`。

## 2. 构建

```powershell
cd I:\CH32H417\dhcp-client
powershell -ExecutionPolicy Bypass -File target/build.ps1 -What all
```

- `-What all`：V3F + V5F + Merge（默认）
- `-What v5f` / `-What v3f`：只构建单个核
- `-What min`：单核最小串口测试（早期验证用）

工具链：`I:\CH32H417\tools\xpack-riscv-none-elf-gcc-15.2.0-1\...\bin`
编译选项：`-march=rv32imafc_zicsr -mabi=ilp32 -Os -ffunction-sections -fdata-sections`
（`libwchnet.a` 是 soft-float ABI，必须用 `ilp32`；`imafc` 是因为厂商 `core_riscv.c`
使用了 `frm/fcsr` 等 FPU CSR。）

### 产物

| 文件 | 大小 | 说明 |
|---|---|---|
| `target/build/v3f.elf/.hex/.bin` | 50180 B bin | V3F helper，@0x00000000 |
| `target/build/v5f.elf/.hex/.bin` | 88912 B bin | V5F 应用，@0x00010000 |
| `target/build/Merge.bin` | 154448 B | **烧录用**，基址 0x08000000 |
| `target/build/Merge.hex` | 434470 B | 同上的 Intel HEX（扩展地址段 = 0x0800） |
| `target/build/Merge_0x0.hex` | 434432 B | 备用：以 0x00000000 为基址的 HEX |

`Merge.bin` 布局：`0x00000..0x0C3C3` = V3F，`0x0C3C4..0x0FFFF` 填 `0xFF`，
`0x10000..0x25B4F` = V5F。

## 3. 烧录

WCH-LinkUtility（`tools/WCHLU/WCH-LinkUtility`）：

1. `Series` 选择 **CH32H41X**（下拉里第 22 项；默认是 CH32V30X，**必须改**）。
2. `TargetFile` 选择 `target/build/Merge.hex`。
3. 点 `Program`（菜单 `Target → Program`，快捷键 **F10**；F9 是 Erase Chip）。
4. 烧完点 `Reset and Run`（F12）。

如果用 `Merge.bin`：在 `Addr` 里填 `0x08000000` 再 Program。

> 之前只有 V5F 的 min.hex 被烧到 0x10000，没有 V3F 去唤醒 V5F，所以芯片不启动——
> 必须烧完整的 Merge 镜像。

## 4. 串口

| 核 | printf | 说明 |
|---|---|---|
| V3F | UART1 | `Core_V3F` → `DEBUG_UART1` |
| V5F | UART8 | `Core_V5F` → `DEBUG_UART8` |

COM3 是 WCH-Link 的 USB 串口，115200 8N1。

### 预期日志（V5F，UART8）

```
=== ch32h417 DHCP client (V5F) ===
SystemClk=... CoreClk=...
mac=xx:xx:xx:xx:xx:xx
ETH_LibInit=0x00 netver=0x1b ok
net stack ready
dhcp start=0 state=SELECTING
dhcp: BOUND
  ip  =192.168.x.y
  mask=255.255.255.0
  gw  =192.168.x.1
  ...
t=5s BOUND ip=192.168.x.y | d=1 r=1 off=1 ack=1 nak=0 bad=0 ign=0
t=10s BOUND ip=192.168.x.y | ...
```

### 预期日志（V3F，UART1）

```
=== ch32h417 dual-core helper (V3F) ===
SystemClk=... CoreClk=...
V5F woken at 0x00010000, V3F parking
```

## 5. 当前状态

- ✅ 双核镜像构建完成，入口/填充/段布局已验证（见 README 末尾的验证记录）。
- ⏳ **等待物理复位/拔插 WCH-Link**：目标核尚未建立调试连接，烧录未执行。
- 烧录后需要确认的点见 `../README.md` 第 12 节硬件验证清单。

## 6. 已知风险

1. `DHCP_APPLY_MODE` 默认 **0**：只打印租约，不改网络栈。确认 DHCP 能通之后再改成 1
   （方案 A：BOUND 后复位一次，用租约地址重新 `ETH_LibInit`）。
2. V5F 不调用 `SystemInit()`（时钟由 V3F 配置），与厂商 V5F 例程一致；若 V5F 单独运行
   需要自行补 `SystemInit()`。
3. `WCHNET_HandleGlobalInt` 的等价实现已跳过 DHCP socket，**不要**换回厂商
   `hardware.c` 里的 `DataLoopback` 版本，否则 DHCP 报文会被回环吞掉。
4. 代码在 RAM 执行（`RAM_CODE`），V5F bss ≈ 31 KB，RAM 256 KB 够用。
