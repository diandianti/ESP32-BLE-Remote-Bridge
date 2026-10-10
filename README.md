# ESP32 BLE Remote Bridge

ESP32-S3（ESP-IDF v6.1）固件：把**蓝牙遥控器**接到电脑上，免驱、免装软件。

遥控器通过 BLE 连到 ESP32-S3（按键走标准 HOGP，语音走厂商私有的 ATVV 协议），
ESP32-S3 再以一个 **USB 复合设备** 的身份接入 Windows，对外提供三样东西：

* **USB HID 键盘 + 多媒体键** —— 遥控器的按键变成 Windows 上的方向键 / 回车 / 多媒体 / 音量键；
* **USB Audio Class 1.0 麦克风** —— 遥控器自带的麦克风变成 Windows 的一个录音设备；
* **WebUSB 厂商接口** —— 用浏览器打开配置站点即可改键位、配对、看日志、甚至在线烧录固件。

上游是 [ncmro7/MI-RC003-ESP32-Bridge](https://github.com/ncmro7/MI-RC003-ESP32-Bridge)（原本只支持小米 RC003）。
本仓库是它的分支（fork），在原有基础上做了大量扩展，**目前支持两种遥控器**。

> **配置站点（WebUSB）**：<https://diandianti.github.io/ESP32-BLE-Remote-Bridge/>
> **在线烧录**：<https://diandianti.github.io/ESP32-BLE-Remote-Bridge/flash/>

> [!WARNING]
> **⚠️ 本项目由 AI 辅助开发，尚未完善，可能存在较多问题，请谨慎使用。**
> 如需稳定可用的方案，建议使用
> [cuicui-V5/RemoteMapper-ESP32](https://github.com/cuicui-V5/RemoteMapper-ESP32)。

---

## 目录

1. [这是什么](#1-这是什么)
2. [支持的遥控器与按键对照](#2-支持的遥控器与按键对照)
3. [硬件](#3-硬件)
4. [烧录固件](#4-烧录固件)
5. [配置站点怎么用](#5-配置站点怎么用)
6. [麦克风](#6-麦克风)
7. [Flash 磨损](#7-flash-磨损)
8. [排查速查表](#8-排查速查表)
9. [编译与发布（贡献者）](#9-编译与发布贡献者)
10. [目录结构](#10-目录结构)
11. [致谢与上游](#11-致谢与上游)
12. [English summary](#12-english-summary)

---

## 1. 这是什么

它解决的问题：

1. Windows 的蓝牙键盘驱动会丢弃遥控器发出的 `音量+`、`音量−`、`返回` 等非标准键码；
2. 语音走厂商私有的 ATVV 协议，纯软件方案要装虚拟声卡；
3. 原方案要求 ESP32 开 Wi-Fi 热点、用户切到那个热点才能配置，体验割裂。

本方案把遥控器当成一个普通的 USB 键盘 + USB 麦克风交给 Windows，配置则用浏览器直连设备。

### 主要特性

| 能力 | 说明 |
| :--- | :--- |
| BLE 直连 | 作为 BLE Central 连接遥控器（HOGP 按键 + ATVV 语音），支持扫描 / 绑定 / 重连 |
| 双遥控器 | 小米蓝牙遥控器 2 Pro（RC003）与 Google TV Remote（ZTKA-IR57），见第 2 节 |
| USB 复合设备 | UAC 1.0 麦克风 + HID 键盘/多媒体/鼠标 + WebUSB 厂商接口，共 4 个接口 |
| 语音麦克风 | IMA-ADPCM 解码 + AGC，语音键触发推流（Google TV 遥控器默认按一下开始 / 再按一下结束，可在「语音设置」改为按住说话）；采样率见 `main/app_config.h` 的 `AUDIO_SAMPLE_RATE`（8 kHz） |
| 多配置方案 | 5 套配置（默认 + 配置 1~4），各配置的按键映射相互独立，支持「穿透继承」默认配置 |
| 动作类型 | 键盘（单击 / 按住 / 释放）、多媒体、鼠标按键、鼠标移动、鼠标滚轮、语音、进入配置切换模式 |
| 手势 | 单击 / 长按 / 双击 / 连发，每种手势可独立配置 |
| 配置切换模式 | 不开网页也能在遥控器上切配置：默认长按电视键进入，方向键选择，指示灯呼吸提示 |
| 浏览器配置 | 纯静态站点，WebUSB 直连；可视化遥控器、可视化键盘、快捷预设、实时状态 |
| 按键学习 | 按下遥控器上的键 → 页面显示它落在哪个物理槽位 → 可以给它起名字 |
| 一键烧录 | 网页在线烧录（ESP Web Tools）+ Windows 免安装 `flash.bat`（内置 esptool） |

---

## 2. 支持的遥控器与按键对照

| 遥控器 | 型号 | 广播名 | 说明 |
| :--- | :--- | :--- | :--- |
| 小米蓝牙语音遥控器 2 Pro | RC003 | `MI RC…` 前缀 | 上游原本就支持的型号 |
| Google TV Remote | ZTKA-IR57 | `Google TV Remote` | 本分支新增，适配细节见 [`docs/GOOGLE-TV-REMOTE-ZTKA-IR57.md`](./docs/GOOGLE-TV-REMOTE-ZTKA-IR57.md) |

### 2.1 Google TV Remote（ZTKA-IR57）按键对照

**这台遥控器暴露了两条 HID Report 特征，各自有互相独立的按键编号空间。**
这是它最容易踩的坑：索引 `1` 在主报告里是「方向上」，在副报告里是「音量+」；
索引 `2` 在主报告里是「方向下」，在副报告里是「音量−」。
把两条报告的索引拍平成一张表，结果是按「音量+」跑出「电源」的动作、按「方向下」跑出「语音」的动作。
完整说明与实现位置见 [`docs/GOOGLE-TV-REMOTE-ZTKA-IR57.md`](./docs/GOOGLE-TV-REMOTE-ZTKA-IR57.md) 第 1 节。

| 报告 | 特征句柄 | 报文形状 | 索引 → 按键 |
| :--- | :--- | :--- | :--- |
| 主报告 | `0x0021` | `[idx, 0]`（2 字节） | 1 上 · 2 下 · 3 左 · 4 右 · 5 确认 · 6 返回 · 7 home · 9 静音 · 10 YouTube · 11 Netflix · 12 电源 · 13 输入源 |
| 副报告 | `0x001D` | `[idx, 0, 0, 0, 0, 0]`（6 字节） | 1 音量+ · 2 音量− |

主报告索引 `8` 本遥控器未使用；副报告只有 `1` 和 `2`。

**语音键不在 HID 报告里。** 它不发任何 HID 报文，只在 ATVV 控制特征 `0x003A` 上发一个
`AUDIO_START`（`0x04`）。固件把它转成一次 `MI_KEY_VOICE` 按下。

### 2.2 物理槽位 vs 遥控器索引

按键「身份」在固件里是**物理槽位（slot）**：共 16 个，顺序与配置站点的 `PHYSICAL_KEYS` 一致。
遥控器的原始索引只是**映射到**槽位，两种遥控器会落到同一套槽位上，
所以内置槽位名与出厂键位映射对两者都通用。

> 配置站点「按键学习」页**不再显示任何「索引」数值**。索引只在单条 HID 报告内唯一，
> 像 ZTKA-IR57 这种双报告遥控器上根本无法由一个数字表示；页面上显示的槽位才是稳定的身份。
> 需要看报文原始索引时请以串口日志 `HOGP: button chr#N idx=M -> key 0xXX (name) DOWN` 为准
> （`chr#N` = 报告通道，`idx=M` = 它在该报告里的索引），在配置站点「运行日志」页打开
> `raw report` 也能看到同一行。

### 2.3 出厂默认键位映射

以下是**默认配置（配置 0）**的出厂映射，取自 `main/keymap/key_state_machine.c`；
全部可在配置站点里改。

| 槽位 | 内置槽位名 | 单次按下 / 单击 | 长按 | 连发 |
| :--- | :--- | :--- | :--- | :--- |
| 0 | 电源键 | `Alt + Tab` | 系统休眠（多媒体 Sleep） | — |
| 1 | 语音键 / 助手 | 开始麦克风推流 + `右Alt + ,` | — | — |
| 2 / 3 / 4 / 5 | 方向 上 / 下 / 左 / 右 | 方向键 上 / 下 / 左 / 右（按住持续） | — | — |
| 6 | 确定键 | 回车 | — | — |
| 7 | 返回键 | 多媒体「返回」(AC Back) | — | — |
| 8 | 主页键 | `Win + D` | — | — |
| 9 | 菜单键 | 空格 | — | — |
| 10 / 11 | 音量 + / − | 音量加 / 减 | — | 是（350 ms 后每 70 ms 一次） |
| 12 | 输入源键 | 多媒体「搜索」(AC Search) | 进入配置切换模式 | — |
| 13 | 静音键 | 多媒体「静音」 | — | — |
| 14 | 预置应用 1 | 多媒体「播放 / 暂停」 | — | — |
| 15 | 预置应用 2 | 多媒体「下一曲」 | — | — |

注意：

* 「菜单键」（槽位 9）在 ZTKA-IR57 上**没有对应的物理按键**，它保留是为了兼容 RC003 的键码；
  该遥控器实际只上报 15 个按键（见 `docs/GOOGLE-TV-REMOTE-ZTKA-IR57.md` 的遥控器信息表）。
* 「静音键 / 预置应用 1 / 预置应用 2」（槽位 13~15）只有部分遥控器变体才有；
  没有的机型永远不会上报这些码。
* 「确定键」在配置切换模式里被锁定为「切回默认配置」，不可改。

---

## 3. 硬件

* **ESP32-S3-WROOM-1 开发板**，支持以下三种板型，请选与实物匹配的 profile 编译 / 烧录：

  | 板型 | Flash | PSRAM | profile | 应用分区 |
  | :--- | :--- | :--- | :--- | :--- |
  | N16R8（默认 / 推荐） | 16 MB | 8 MB Octal | `n16r8` | 3 MB（`partitions.csv`） |
  | N8R2 | 8 MB | 2 MB Quad | `n8r2` | 5 MB（`partitions_8mb.csv`） |
  | N4R2 | 4 MB | 2 MB Quad | `n4r2` | 2.75 MB（`partitions_4mb.csv`） |

* 板载 WS2812 RGB 指示灯，默认 GPIO48（可在 `main/app_config.h` 改 `RGB_BUILTIN`）。
* 一根（或两根）Type-C 数据线：一根接 **USB/OTG** 口（必须，复合设备与 WebUSB 都走它），
  UART 口可选，用于看串口日志 / 烧录。

### 怎么知道自己是哪种板

优先级从高到低：

1. **问芯片**（最可靠）：进入 ROM 下载模式后执行

   ```powershell
   esptool.py --port COM5 flash_id     # 输出 Flash 容量，如 "Detected flash size: 16MB"
   ```

   PSRAM 容量可用 `idf.py -p COM5 monitor` 看启动日志里的 `SPIRAM` 一行。
2. **看模组丝印 / 商品页**：`ESP32-S3-WROOM-1-N16R8`、`-N8R2`、`-N4R2` 分别对应上表。
3. 拿不准时**用 N4R2 的最小分区**烧 N16R8 板子也能跑（只是浪费空间），
   反过来（把 N16R8 固件烧进 4 MB 板）一定失败。

固件不通用：Flash 容量与 PSRAM 模式（Octal / Quad）都编在镜像里。

---

## 4. 烧录固件

### 4.1 网页在线烧录（推荐）

打开 <https://diandianti.github.io/ESP32-BLE-Remote-Bridge/flash/>：

1. 在「选择硬件版本」里选对应板型（N16R8 / N8R2 / N4R2）；
2. 用数据线把开发板接到电脑；
3. 点「连接设备并烧录」，在串口列表里选开发板；
4. 首次烧录会问是否擦除 Flash —— 选「擦除」；
5. 完成后设备自动重启，回到 <https://diandianti.github.io/ESP32-BLE-Remote-Bridge/> 配置。

要求：**桌面版 Chrome / Edge**（Web Serial），页面必须处于安全上下文（`https://` 或 `http://localhost`）。

### 4.2 Windows 免安装工具

从 [Releases](https://github.com/diandianti/ESP32-BLE-Remote-Bridge/releases) 下载
`MI-RC003-Bridge-<版本>-win64.zip`（文件名沿用历史命名），解压后双击 `flash.bat`。
内置 esptool，**不需要 Python / ESP-IDF**，会列出可用板型并自动探测串口：

```bat
flash.bat -Profile n8r2      :: 直接指定板型，跳过选择
flash.bat -Port COM5         :: 指定串口
flash.bat -Erase             :: 烧录前整片擦除
```

设备正在跑固件时，原生 USB 口**不会出现 COM 口**（它此时是 UAC + HID + WebUSB 复合设备）。
先按住 `BOOT` → 点按 `RST` → 松开 `BOOT` 进入 ROM 下载模式，工具会等待最多 90 秒。
也可以改用板载 UART 口（GPIO43/44，需 CH34x / CP210x 驱动）烧录。

### 4.3 从源码编译（ESP-IDF v6.1）

```powershell
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"   # 激活 ESP-IDF 环境
idf.py set-target esp32s3                                      # 首次 / 换芯片

# N16R8（默认）
idf.py build

# N8R2 / N4R2：用 -D 指定该板型的 sdkconfig 与默认值（分区表也在这里面切换）
idf.py -D "SDKCONFIG=.sdkconfig.n8r2" -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.n8r2" build
idf.py -D "SDKCONFIG=.sdkconfig.n4r2" -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.n4r2" build

idf.py -p COMx flash monitor
```

首次编译会通过 IDF Component Manager 自动下载依赖：
`espressif/esp_tinyusb`、`espressif/tinyusb`、`bblanchon/arduinojson`、`espressif/led_strip`。

也可以直接用仓库脚本一次编完所有板型：

```bat
build-firmware.bat                  :: 构建 n16r8 / n8r2 / n4r2
build-firmware.bat -Profile n8r2    :: 仅构建指定板型（可逗号分隔）
build-firmware.bat -NoBuild         :: 跳过编译，只用现有 build 产物重新合并
```

`ESP-IDF` 环境路径、编译 / 发布约定见 [`AGENTS.md`](./AGENTS.md)。

---

## 5. 配置站点怎么用

配置页面在 [`webusb-config/`](./webusb-config)，是一个**纯静态站点**，没有后端。
部署地址：<https://diandianti.github.io/ESP32-BLE-Remote-Bridge/>

连接步骤：

1. 用 USB 数据线把开发板接到电脑（建议原生 USB/OTG 口）；
2. 桌面版 Chrome / Edge 打开上面的地址；
3. 点「连接设备」，在弹窗里选择本设备的 USB 产品名 **`MI-RC003 Remote Bridge`**；
4. 首次使用先配对遥控器：拿起遥控器，同时按住「主页键 + 菜单键」约 3 秒进入配对广播，
   然后在「蓝牙配对」页扫描并连接。

> USB 产品名 / 序列号是设备身份，由 `main/usb/usb_descriptors.c` 的字符串描述符定义，
> 本轮改名**没有**动它（见第 11 节）。Chrome 的设备选择框里显示的就是这个名字。

页面标签页：

| 标签页 | 能做什么 |
| :--- | :--- |
| **设备状态** | 固件版本、构建时间、运行时间、BLE 状态、遥控器电量、当前配置、切换模式、音频帧数、内存 / PSRAM；下方是实时按键检测（当前按下的键、最近动作、键值、时长） |
| **按键配置** | 5 套配置方案的可视化遥控器编辑；点任意按键可配单击 / 长按 / 双击 / 连发，动作可选可视化键盘、扩展按键、多媒体、鼠标按键 / 移动 / 滚轮、语音快捷键；另有「配置切换模式…」弹窗编辑切换映射 |
| **按键学习** | 按一下遥控器上的键 → 顶部读数显示它是第几个**物理槽位** → 在表格里给它起名字（存在设备 NVS 里）→ 「检测」可确认某行对应哪个键，「去映射」跳到「按键配置」改这个槽位的动作 |
| **蓝牙配对** | 扫描 / 连接 / 重新连接 / 解除绑定 |
| **运行日志** | 查看 / 清空设备日志（自动刷新） |
| **系统设置** | 重启、恢复出厂（清空 NVS）、原始 JSON 导入导出 |

### 5.1 按键学习

* 名称**只影响显示**：让你自己看得懂哪个槽位对应遥控器上的哪个键；这个槽位实际发出什么按键，
  仍然由「按键配置」里的映射决定，两者互不影响。
* 名称按 **UTF-8 字节**计算，上限 **31 字节**（约 10 个汉字）。超出部分在保存时按**字符边界**截断，
  不会留下半个汉字。留空 = 恢复内置名称。
* 名字按设备保存，换一台遥控器 / 清空 NVS 后回到内置名称。
* 内置名称是**按标准 Google TV 遥控器的顺序猜的**。索引顺序不同的型号默认名会对不上实物 ——
  以实测为准：**按下哪个键，亮起来的就是它**，然后改名即可。

### 5.2 本地运行（二次开发）

WebUSB 要求安全上下文，`http://localhost` 被视为安全来源：

```powershell
cd webusb-config
python -m http.server 8000
```

浏览器打开 <http://localhost:8000/>。此时需要把固件 `main/version.h` 的
`WEBUSB_LANDING_URL` / `WEBUSB_LANDING_SCHEME` 临时改成 `localhost:8000/` 与 `0` 重新编译
（这两个值只影响 USB BOS 描述符里公示的着陆页地址，不影响功能）。

设备通信已封装为无依赖的浏览器库 [`webusb-config/assets/mi-rc003.js`](./webusb-config/assets/mi-rc003.js)
（挂载全局 `MiRC003`）。第三方可以只引这个库、自己写 UI：

* API 参考（权威，随库维护）：[`webusb-config/api.md`](./webusb-config/api.md)
* 兼容性：桌面版 Chrome / Edge（Windows / macOS / Linux）；移动端不支持 WebUSB；
  Windows 10/11 会自动为带 BOS 描述符的厂商接口绑定 WinUSB（**无需 Zadig**）；
  Linux 需要 udev 规则。

---

## 6. 麦克风

### 6.1 链路

```text
遥控器 --ADPCM 8 kHz--> BLE ATVV 特征 0x0037
   -> atvv_audio.c 判定封装（ZTKA-IR57 是"裸流"，没有 ATVV 头）
   -> audio_pipeline_feed_adpcm()   解码 + DSP + 环形缓冲
   -> audio_pipeline_read_for_usb()
   -> uac_microphone.c  uac_stream_task()   每 2 ms 提交一包
   -> USB 等时 IN 端点 0x81  ->  Windows
```

测得的速率：每包 14–20 字节、约 200 包/秒 → 约 4000 B/s，正好是 8 kHz / 16 bit @ 4:1，
所以固件按 `AUDIO_SAMPLE_RATE 8000` 构建（`main/app_config.h`）。
`MIC_OPEN` 请求的是 codec `0x0002`（16 kHz），遥控器实际用 `codec 1`（8 kHz）回，**以遥控器为准**。

### 6.2 主机不录音，固件一个字节都发不出去

UAC 采集是**主机驱动**的：只有应用真的开始录音，Windows 才会发 `SET_INTERFACE alt=1`；
在此之前端点停在 `alt=0`（零带宽）。这不是故障，是 USB 音频的规矩。

所以排查看两点：日志里有没有 `UAC: Host SET_INTERFACE itf=1 alt=1`，
以及 `AUDIO: Session:` 行的 `usb_reads` 是否 `> 0`。

### 6.3 重要：USB 序列号里编了采样率，不要手工改

Windows 把每个录音端点的**默认格式**缓存进注册表，按**设备实例（VID/PID/序列号）**索引，
并且**用这个缓存格式以共享模式打开采集流**。改了描述符里的采样率却没有换设备身份，就会出现：

| | |
| :--- | :--- |
| Windows 缓存 | 16000 Hz / 1 ch |
| 设备声明 | 8000 Hz / 1 ch |
| 结果 | `IAudioClient::Initialize` 失败 → 应用报 `NotReadableError` → **主机连 `alt=1` 都不发** |

特征现象：日志里**一次 `alt=1` 都没有**，`AUDIO: Session:` 的 `usb_reads=0`，
但 `peak_out` 是正常语音电平。

固件的处理方式（`main/usb/usb_descriptors.c`）：**把采样率编进 USB 序列号**，
格式一变就等于换了新硬件，Windows 会按当前描述符重建端点：

```text
84C7BB5A7004-8000        # MAC + AUDIO_SAMPLE_RATE
```

**改采样率时不需要手工做任何事**，这是自动的。副作用是旧端点会变成 `NOT PRESENT` 的幽灵项
（不出现在声音设置里）。**重新实现 / 移植这套固件的人必须知道这一点**：
只要动了采集格式，就必须同时改设备身份，否则 Windows 侧永远打不开流。

### 6.4 Windows 侧

* 录音设备名是 **`MI-RC003 Microphone`**（`main/usb/usb_descriptors.c` 里的字符串描述符）；
* 语音键默认发 `右Alt + ,`，要和输入法的语音热键一致（可在「按键配置」里改）；
* 快捷键的发送方式在「按键配置 → 语音设置…」里选：全程按住 / 开始结束各按一下 / 只在开始按一下。
  全程按住时右 Alt 在整个录音期间都是按下的，此时键盘上按的其它键会变成组合键；输入法支持
  「按一下开关」时建议改为「开始结束各按一下」。「单次语音最长时长」到时会自动结束录音；
* 想绕开 Windows 设置页面的电平表直接验证采集流，用仓库里的
  [`webusb-config/mic-test.html`](./webusb-config/mic-test.html)（本地 `http://localhost` 打开）。

---

## 7. Flash 磨损

设计原则：**运行时状态不落盘，只有配置落盘。**

| 写入路径 | 触发 | 处理 |
| :--- | :--- | :--- |
| NVS 布局标记 | 仅首次 | 一次 |
| BLE 配对信息 | 仅绑定 MAC 变化（= 重配） | 已有变化检测 |
| 键盘映射保存 | 网页改配置 | 逐键比对，**无变化连 `nvs_commit` 都不调用** |
| 切层 | 每次切层 | **完全不写**（早期版本会重写整个键盘映射） |
| 按键名称 | 改名 | 归一化后比对，**没变不写**；连续改名合并成一次（200 ms 防抖） |

**不写 flash 的：** 日志（`app_log` 只在 RAM）、遥测 / 状态轮询、电量刷新（GATT 读）、
音频统计、LED 状态。

NimBLE 的配对库（`CONFIG_BT_NIMBLE_NVS_PERSIST=y`）绕过 `config_store` 直接写 NVS，
实测重连 + 加密前后 NVS 条目数**不变**，即重连不重写 bond。

### 磨损量表：HEALTH 行的 `nvs=`

开机和每 5 秒的 HEALTH 行都带 NVS 已用条目数：

```text
INIT: NVS: boot: used=156 free=1860 total=2016 entries
HEALTH: rx=0 total=0 release=0 gtv=0 rc003=0 layout=google_tv nvs=156
```

读法：**正常使用时它不动 = 没有东西在写 flash**；改配置时会跳一下。
固件故意用分区级统计（`nvs_get_stats`）而不是自己计数 —— 自己数看不到 NimBLE 的写入。

---

## 8. 排查速查表

### 8.1 日志行看什么

| 日志行 | 看什么 |
| :--- | :--- |
| `HOGP: button chr#N idx=M -> key 0xXX (name) DOWN` | `chr#1` = 主报告，`chr#0` = 副报告。索引对不上就查第 2.1 节 |
| `HOGP: rx#N len=.. [..]` | 前 32 次按键的原始报文 |
| `LEARN: chr#N len=.. [..]` | 打开 raw report 后，每次**不同**报文打一行（按住不刷屏） |
| `ATVV: Voice start (session N, codec X)` | 语音键按下（ATVV 路径） |
| `ATVV: audio: N packet(s) -> 0 frame(s)` | **`0 frame(s)` 是正常的**：它只统计带 ATVV 头的帧，ZTKA-IR57 发的是裸流 |
| `AUDIO: Session: blocks=.. queued=.. peak_in=.. peak_out=.. usb_reads=..` | `peak_out` 看解码是否出音，`usb_reads` 看主机是否在取数据 |
| `UAC: Host SET_INTERFACE itf=1 alt=N` | `alt=1` 才代表主机开始采集 |
| `HEALTH: … nvs=N` | flash 磨损量表（第 7 节） |

### 8.2 麦克风诊断矩阵

`AUDIO: Session:` 四种组合：

| 有 `alt=1` | `peak_out` | `usb_reads` | 结论 |
| :--- | :--- | :--- | :--- |
| 否 | – | 0 | 主机没开始采集 —— Windows 侧问题，见第 6.3 节 |
| 是 | 0 | >0 | 解码出来是静音 → 查解码 / DSP |
| 是 | >0 | 0 | 流传开了但没喂端点 |
| 是 | >0 | >0 | 音频已在送，问题在主机侧音量 / 应用 |

### 8.3 其它常见问题

**烧录工具检测不到串口** —— 设备正跑固件时原生 USB 口不提供串口；
先 `BOOT` → `RST` → 松 `BOOT` 进下载模式（设备管理器里会出现 `USB JTAG serial debug unit` 并分配 COM 口），
工具会自动等待最多 90 秒。也可改用板载 UART 口（GPIO43/44）。

**设备管理器提示「代码 28 / 驱动程序未安装」** —— 固件在 BOS 描述符里同时提供 WebUSB 与
Microsoft OS 2.0（WINUSB 兼容 ID）描述符，Windows 10/11 应自动加载 `winusb.sys`。
若仍报错：确认烧的是最新固件（PID `0x8304`），在设备管理器卸载残留旧设备后重新扫描；无需 Zadig。

**串口日志出现 `NIMBLE_NVS: NVS data size mismatch`** —— 之前烧过 Arduino 版 RemoteMapper，
其 NimBLE 绑定数据结构与本固件不同。固件内置一次性 NVS 迁移；仍有问题就 `idf.py erase-flash` 后重烧。

**浏览器找不到设备** —— 必须是桌面版 Chrome / Edge；页面必须处于 `https://` 或 `http://localhost`；
关掉可能占用该设备的程序（串口助手、Zadig）；首次使用在弹窗里选 `MI-RC003 Remote Bridge`。

**语音键没声音** —— 先在 Windows 声音设置里把输入设备切成 `MI-RC003 Microphone`，
并确认输入法语音热键与遥控器语音键配置一致（默认 `右Alt + ,`）。

---

## 9. 编译与发布（贡献者）

本节是 [`AGENTS.md`](./AGENTS.md) 的摘要，**以 `AGENTS.md` 为准**。

### 9.1 编译

* ESP-IDF **v6.1**，目标 `esp32s3`；激活环境：

  ```powershell
  . "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
  ```

  若该 profile 不存在（EIM 安装布局），改用 `export.ps1` 并显式设置
  `IDF_PATH` / `IDF_TOOLS_PATH` / `IDF_PYTHON_ENV_PATH`（见 `AGENTS.md` 第 2 节）。
* 日常编译 `idf.py build`；换芯片先 `idf.py set-target esp32s3`。
* **不要提交** `build/`、`sdkconfig`、`managed_components/`、`dist/`、`.cache/`（已在 `.gitignore` 里）。

### 9.2 发布流程

发布分三步，**全部用仓库内脚本，不要在脚本之外手工拼装固件**：

```bat
build-firmware.bat      :: 1. 编译 + 生成各板型的烧录固件（Windows 工具用 + 网页烧录用）
package-release.bat     :: 2. 打包 Windows 免安装烧录工具到 dist\
                        :: 3. 手工发布 GitHub Release（见下）
```

| 产物 | 说明 |
| :--- | :--- |
| `build/firmware/merged-flash-<板型>.bin` | 各板型的合并固件（含 bootloader + 分区表 + 应用），Windows 烧录工具用；默认板型另存一份 `build/merged-flash.bin` |
| `webusb-config/flash/firmware/merged-flash-<板型>.bin`、`manifest-<板型>.json`、`boards.json` | 网页烧录固件与板型清单，**受版本控制**，发布时要一并提交 |
| `dist/MI-RC003-Bridge-<版本>-win64/` | 免安装 Windows 烧录包（内置 esptool + `flash.bat` + `使用说明.txt`） |
| `dist/MI-RC003-Bridge-<版本>-win64.zip` | 上传 GitHub Release 的压缩包 |
| `dist/SHA256SUMS.txt` | 发布包 SHA-256 |

发布 GitHub Release 时，除 zip 外**必须一并上传全部原始固件**（按板型，便于单独烧录 / 回滚）：
`build/firmware/merged-flash-n16r8.bin`、`-n8r2.bin`、`-n4r2.bin`，以及 `build/merged-flash.bin`。

### 9.3 版本号与着陆页

* 版本号**唯一来源**是 `main/version.h` 的 `FIRMWARE_VERSION`，发布前在这里递增。
  `build-firmware.ps1` / `package-release.ps1` 会自动读取并写进 manifest，
  **不要在任何其它文件里单独维护版本号**。
* `main/version.h` 的 `WEBUSB_LANDING_URL` / `WEBUSB_LANDING_SCHEME`
  必须与 `webusb-config/` 的部署地址一致。当前值：

  ```c
  #define WEBUSB_LANDING_URL     "diandianti.github.io/ESP32-BLE-Remote-Bridge/"
  #define WEBUSB_LANDING_SCHEME  1   // 0 = http, 1 = https
  ```

* 推到 `main` 后 GitHub Pages 自动部署（[`.github/workflows/static.yml`](./.github/workflows/static.yml)，
  发布目录就是 `webusb-config/`）。

### 9.4 发行包命名与 CMake 目标名

* 发行包名（`MI-RC003-Bridge-<版本>-win64`）与 CMake 工程名（`project(mi_rc003_bridge)`）
  **刻意保持 MI-RC003 的历史命名**：`flash_app.ps1` / `flash_s3.ps1` 等脚本与
  `tools/` 下的发布流程都按 `mi_rc003_bridge.bin` 这个名字找产物。
  改名会让脚本与既有 Releases 失效，所以本次只改用户可见的项目名，不动这两处。
* 同理，`webusb-config/assets/mi-rc003.js`（库文件名）与 USB 描述符里的设备名
  （`MI-RC003 Remote Bridge` / `MI-RC003 Microphone`）都保持原样。

### 9.5 WebUSB 库的维护要求

* 权威文档是 [`webusb-config/api.md`](./webusb-config/api.md)；任何公开 API 变更必须同步更新它。
* 常量必须与固件一致（`MiRC003.CMD` ↔ `main/webusb/webusb_protocol.h` 的 `CMD_*`，
  `MiRC003.ACTIONS` ↔ `main/keymap/key_state_machine.h` 的 `ACTION_*`），改一端必须同步另一端。
* 改完库要跑语法检查：`node --check webusb-config/assets/mi-rc003.js`。

---

## 10. 目录结构

```text
ESP32-BLE-Remote-Bridge/
├── CMakeLists.txt                             # 工程名 mi_rc003_bridge（勿改，见 9.4）
├── sdkconfig.defaults                         # 基础配置（N16R8 / 16MB + 8MB Octal）
├── sdkconfig.defaults.{n16r8,n8r2,n4r2}       # 各板型覆盖（Flash / PSRAM / 分区表）
├── partitions.csv / partitions_8mb.csv / partitions_4mb.csv
├── build-firmware.bat                         # 一键生成烧录固件（Windows 工具 + 网页）
├── package-release.bat                        # 打包 Windows 免安装烧录工具
├── AGENTS.md                                  # 编译 / 发布 / WebUSB API 约定（权威）
├── docs/
│   └── GOOGLE-TV-REMOTE-ZTKA-IR57.md          # ZTKA-IR57 实测记录（按键 / 麦克风 / 排查）
├── tools/
│   ├── common.ps1                             # 脚本公共函数（板型表也在这里）
│   ├── build-firmware.ps1                     # 编译 + 合并固件 + 生成网页 manifest
│   ├── package-release.ps1                    # 打包 dist\ Windows 烧录工具
│   └── standalone/{flash.bat,flash.ps1}       # 最终用户免安装烧录脚本模板
├── main/
│   ├── main.c                                 # 初始化与任务
│   ├── app_config.h / version.h               # 引脚、音频参数、固件名与版本、着陆页
│   ├── ble/ble_remote_client.*                # NimBLE Central：HOGP 按键 + ATVV 语音
│   ├── audio/                                 # IMA-ADPCM / AGC / 滤波 / 环形缓冲
│   ├── keymap/                                # 多配置按键状态机 + 切换模式 + 槽位名 + NVS
│   ├── storage/config_store.*                 # NVS 封装
│   ├── usb/
│   │   ├── usb_descriptors.*                  # 设备 / 配置 / BOS / WebUSB / HID 描述符
│   │   ├── uac_microphone.*                   # 自定义 UAC 1.0 类驱动
│   │   ├── hid_bridge.*                       # HID 键盘 + 多媒体 + 鼠标发送
│   │   ├── webusb_transport.*                 # 厂商端点帧协议
│   │   └── usb_composite.*
│   ├── webusb/webusb_protocol.cpp             # WebUSB 命令分发
│   ├── led/led_indicator.*                    # RGB 指示灯（状态 / 呼吸）
│   └── log/app_log.*                          # 日志（只在 RAM）
└── webusb-config/                             # 浏览器配置站点（纯静态，GitHub Pages 发布目录）
    ├── index.html
    ├── api.md                                     # 浏览器库 API 参考（权威）
    ├── doc.md                                     # 旧版说明（保留）
    ├── mic-test.html                              # 麦克风采集流测试页
    ├── assets/{mi-rc003.js,app.js,style.css}
    └── flash/                                  # 网页固件烧录（ESP Web Tools）
        ├── index.html
        ├── boards.json                         # 板型清单（页面据此选择硬件版本）
        ├── manifest-<板型>.json
        └── firmware/merged-flash-<板型>.bin
```

---

## 11. 致谢与上游

### 上游

本仓库是 [ncmro7/MI-RC003-ESP32-Bridge](https://github.com/ncmro7/MI-RC003-ESP32-Bridge) 的 **fork**。
上游的 README、设计与署名在本仓库中保留（见第 12 节的英文摘要与 `LICENSE`）。
本分支相对上游的主要变化，逐项记录在 [`docs/`](./docs) 中，其中
[`docs/GOOGLE-TV-REMOTE-ZTKA-IR57.md`](./docs/GOOGLE-TV-REMOTE-ZTKA-IR57.md)
是 ZTKA-IR57 遥控器的实测记录（两条 HID 报告、语音走 ATVV、麦克风链路、采样率与序列号、
flash 磨损、排查速查表，以及被实测推翻的早期结论）。

### 致谢

* [ncmro7/MI-RC003-ESP32-Bridge](https://github.com/ncmro7/MI-RC003-ESP32-Bridge)：本仓库的上游。
* [cuicui-V5/RemoteMapper-ESP32](https://github.com/cuicui-V5/RemoteMapper-ESP32)：硬件桥接架构、按键引擎与音频处理。
* [HD838A/remote-mic-app](https://github.com/HD838A/remote-mic-app)：RC003 的 HOGP / ATVV 协议细节与 UI 参考。
* [QL-4/RemoteMapper](https://github.com/QL-4/RemoteMapper)、[godarrenw/mi_remote_control](https://github.com/godarrenw/mi_remote_control)：协议逆向参考。
* [TinyUSB](https://github.com/hathach/tinyusb) 与 [esp_tinyusb](https://github.com/espressif/esp-usb)：USB 协议栈。
* [ESP Web Tools](https://esphome.github.io/esp-web-tools/)：网页烧录页。

### 开源协议

[MIT License](./LICENSE)，版权归上游与本分支贡献者共同所有。

---

## 12. English summary

**ESP32 BLE Remote Bridge** (ESP-IDF v6.1, ESP32-S3) bridges a Bluetooth remote control to
Windows as a driver-free USB composite device:

* **USB HID keyboard + consumer control** — remote buttons become arrow / Enter / media / volume keys;
* **USB Audio Class 1.0 microphone** — the remote's built-in mic appears as a Windows recording device;
* **WebUSB vendor interface** — a static site (<https://diandianti.github.io/ESP32-BLE-Remote-Bridge/>)
  does key mapping, layers, button learning, BLE pairing, logs and browser-based firmware flashing.

Supported remotes: Xiaomi Bluetooth Remote 2 Pro (**RC003**) and **Google TV Remote (ZTKA-IR57)**.
The ZTKA-IR57 exposes **two** HID report characteristics with independent button-index spaces
(`0x0021`, 2-byte reports: dpad / OK / back / home / mute / YouTube / Netflix / power / input;
`0x001D`, 6-byte reports: volume up / down). The voice button sends no HID report at all — it
arrives as an ATVV `AUDIO_START` on characteristic `0x003A`. Folding the two index spaces into one
table makes volume-up trigger the power action; see
[`docs/GOOGLE-TV-REMOTE-ZTKA-IR57.md`](./docs/GOOGLE-TV-REMOTE-ZTKA-IR57.md) §1.

Hardware profiles: **N16R8** (16 MB flash / 8 MB octal PSRAM, default), **N8R2**, **N4R2**.
Flash the board from the browser page above (desktop Chrome / Edge, secure context), with the
standalone Windows `flash.bat` from Releases, or build from source with ESP-IDF v6.1.

The microphone reaches Windows over the UAC 1.0 isochronous endpoint `0x81`, but only while a
host application actually records: Windows sends `SET_INTERFACE alt=1` and nothing flows before
that. **Changing the audio sample rate requires changing the device identity**: Windows caches each
capture endpoint's default format in the registry keyed on the device instance and reopens the
stream at that cached rate, so a new rate with the old serial makes `IAudioClient::Initialize` fail
and the app reports `NotReadableError` without ever selecting `alt=1`. The firmware therefore
encodes the rate in the USB serial number (`<MAC>-8000`).

Runtime state is never persisted — only configuration is — and the `nvs=` field on the `HEALTH`
log line is the flash-wear gauge.

This repository is a **fork** of
[ncmro7/MI-RC003-ESP32-Bridge](https://github.com/ncmro7/MI-RC003-ESP32-Bridge); upstream
attribution and the MIT licence are preserved. Fork-specific findings are documented under
[`docs/`](./docs). The CMake project name (`mi_rc003_bridge`), the release package name
(`MI-RC003-Bridge-<version>-win64`) and the USB descriptor strings are intentionally unchanged,
because the flashing and release scripts reference them by name.

Build and release: `build-firmware.bat` → `package-release.bat` → publish a GitHub Release
(the version's single source of truth is `FIRMWARE_VERSION` in `main/version.h`); see
[`AGENTS.md`](./AGENTS.md).
