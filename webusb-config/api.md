# MI-RC003 Bridge · WebUSB API 参考

> 本文档是 `webusb-config/assets/mi-rc003.js` 的**权威 API 参考**。
>
> - 库文件：[`assets/mi-rc003.js`](./assets/mi-rc003.js)
> - 默认界面示例：[`assets/app.js`](./assets/app.js)
> - 旧版说明（保留）：[`doc.md`](./doc.md)

---

## 目录

1. [加载与快速开始](#1-加载与快速开始)
2. [常量](#2-常量)
3. [`MiRC003` 客户端](#3-mirc003-客户端)
4. [`MiRC003.Keymap` 工具](#4-mirc003keymap-工具)
5. [Keymap JSON 结构](#5-keymap-json-结构)
6. [WebUSB 线协议](#6-webusb-线协议)
7. [错误处理](#7-错误处理)
8. [兼容性](#8-兼容性)
9. [维护约定](#9-维护约定)

---

## 1. 加载与快速开始

`mi-rc003.js` 是一个无依赖的浏览器端脚本，挂载全局 `MiRC003`。

```html
<script src="assets/mi-rc003.js"></script>
<script>
  const dev = new MiRC003();

  (async () => {
    await dev.connect();                       // 弹出设备选择框
    console.log(await dev.status());           // 读取状态

    const keymap = await dev.getKeymap();
    const layer = MiRC003.Keymap.findLayer(keymap, 0);
    MiRC003.Keymap.setGesture(layer, 0x28, "click",
      MiRC003.Keymap.action(MiRC003.ACTIONS.KEYBOARD_TAP, { keyCode: 0x28 }));
    await dev.saveKeymap(keymap);              // 分块上传并落盘
  })();
</script>
```

> WebUSB 需要**安全上下文**：`https://` 或 `http://localhost`。
> 桌面版 Chrome / Edge 支持，移动端浏览器不支持。

---

## 2. 常量

所有常量挂在 `MiRC003` 上，例如 `MiRC003.CMD.STATUS`。

### `MiRC003.CMD` — 命令字

| 键 | 值 | 说明 |
| :--- | :--- | :--- |
| `DEVICE_INFO` | `0x50` | 设备信息 |
| `STATUS` | `0x01` | 运行状态 |
| `LOGS_GET` | `0x02` | 读取日志 |
| `LOGS_CLEAR` | `0x03` | 清空日志 |
| `KEYMAP_GET` | `0x10` | 读取按键映射 |
| `KEYMAP_SAVE` | `0x11` | 单帧保存（兼容旧接口） |
| `KEYMAP_RESET` | `0x12` | 恢复默认按键 |
| `KEYMAP_TELEMETRY` | `0x13` | 最近按键遥测 |
| `KEYMAP_BEGIN` | `0x15` | 分块上传开始 |
| `KEYMAP_DATA` | `0x16` | 分块上传数据 |
| `KEYMAP_COMMIT` | `0x17` | 分块上传提交 |
| `SET_LAYER` | `0x18` | 切换设备当前配置（层） |
| `BLE_SCAN` | `0x20` | 扫描蓝牙 |
| `BLE_CONNECT` | `0x21` | 连接指定设备 |
| `BLE_UNPAIR` | `0x22` | 解除绑定 |
| `BLE_INFO` | `0x23` | 已绑定信息 |
| `BLE_RECONNECT` | `0x24` | 重新连接 |
| `BLE_RAW_REPORT` | `0x25` | 逐条打印 HID 原始报文（调试用） |
| `BLE_LAYOUT` | `0x26` | 指定 HID 报文方言（`auto` / `rc003` / `google_tv`） |
| `KEY_NAMES_GET` | `0x27` | 读取 16 个物理槽位的自定义名称 |
| `KEY_NAMES_SET` | `0x28` | 修改单个槽位的自定义名称 |
| `NVS_RESET` | `0x31` | 恢复出厂（清空 NVS） |
| `SYSTEM_RESTART` | `0x40` | 重启设备 |

### `MiRC003.STATUS` — 响应状态码

```js
{ OK: 0, ERR_CMD: 1, ERR_ARG: 2, ERR_INTERNAL: 3 }
```

### `MiRC003.ACTIONS` / `MiRC003.ACTION` — 动作类型

`ACTIONS` 是数值常量，`ACTION` 是「类型 → 中文名」映射。

```js
MiRC003.ACTIONS.KEYBOARD_TAP   // 1
MiRC003.ACTIONS.CONSUMER_TAP   // 4
MiRC003.ACTIONS.MOUSE_MOVE     // 14

MiRC003.ACTION[1]              // "键盘-单击"
```

完整数值：

| 名称 | 值 | 名称 | 值 |
| :--- | :--- | :--- | :--- |
| `NONE` | 0 | `SWITCH_LAYER` | 9 |
| `KEYBOARD_TAP` | 1 | `TRANSPARENT` | 10 |
| `KEYBOARD_HOLD` | 2 | `MOUSE_BUTTON_TAP` | 11 |
| `KEYBOARD_RELEASE` | 3 | `MOUSE_BUTTON_HOLD` | 12 |
| `CONSUMER_TAP` | 4 | `MOUSE_BUTTON_RELEASE` | 13 |
| `CONSUMER_HOLD` | 5 | `MOUSE_MOVE` | 14 |
| `CONSUMER_RELEASE` | 6 | `MOUSE_WHEEL` | 15 |
| `VOICE` | 7 | `ENTER_SWITCH_MODE` | 16 |
| `VOICE_RELEASE` | 8 | | |

> `MOUSE_BUTTON_RELEASE`（13）为固件内部使用（松开鼠标按住键时自动发出），
> 默认 UI 不再提供该选项；此处保留常量以便解析遥测数据。
>
> `SWITCH_LAYER`（9，立即切换配置）为旧接口，默认 UI 已不再提供；请改用
> `ENTER_SWITCH_MODE`（16）。此处保留常量以便解析旧配置与遥测数据。

### 语音设置常量（`keymap.voice`）

| 常量 | 值 | 含义 |
| :--- | :--- | :--- |
| `VOICE_TRIGGER.TOGGLE` | 0 | 按一下语音键开始，再按一下结束（默认） |
| `VOICE_TRIGGER.HOLD` | 1 | 按住说话，松开结束（遥控器自身最长约 15 s） |
| `VOICE_HOTKEY_MODE.HOLD` | 0 | 电脑端快捷键在整个会话期间保持按下（默认） |
| `VOICE_HOTKEY_MODE.TAP_BOTH` | 1 | 开始时单击一次，结束时再单击一次 |
| `VOICE_HOTKEY_MODE.TAP_START` | 2 | 只在开始时单击一次 |
| `VOICE_DEFAULTS` | `{ trigger: 0, hotkey_mode: 0, max_sec: 120 }` | 固件出厂值 |
| `VOICE_MAX_SEC_LIMIT` | 3600 | `max_sec` 上限（秒） |

- `trigger` 只对语音键是 ATVV 按键的遥控器（Google TV Remote）生效；小米 RC003 的语音键是
  HID 按键，始终为「按住说话」。
- `hotkey_mode = HOLD` 时修饰键（如右 Alt）在整个会话期间处于按下状态，期间在电脑键盘上按的其它键
  会被系统视为组合键。输入法支持「按一下开关」时应选 `TAP_BOTH`。
- `max_sec` 为单次会话最长时长（0 = 不限），到时固件自动结束会话并松开快捷键。

### 配置切换模式（`ENTER_SWITCH_MODE`）

把某个按键的任一动作设为 `ENTER_SWITCH_MODE` 后，触发该动作会进入**配置切换模式**：

- 进入后屏蔽普通按键输出，指示灯以**当前配置颜色呼吸闪烁**。
- 按全局映射表（见 [`switch_map`](#5-keymap-json-结构)）中的按键会切换到对应配置并自动退出；
  默认映射为：方向上/右/下/左 → 配置 1/2/3/4。「确定键」被固件**锁定为默认配置**，不参与映射表。
- 再次按「当前配置中绑定为进入切换模式」的按键退出（其它配置里的绑定不算数）；「返回键」始终可退出。
  电视键仅在「由连按 5 次进入」或「当前配置绑定为切换键」时才能退出（进入满 2 秒后）。
  约 5 秒无操作自动退出（模式内任意按键都会重新计时）。
- 快速连按「电视键」5 次进入后，2 秒内忽略电视键，避免连按的尾键立即退出。
- 出厂默认把**长按电视键**设为进入切换模式（短按电视键仍为 F8）。
- 兜底：快速连按「电视键」5 次（1.5 秒内）也会进入切换模式，用于当前配置未设置切换按键的情况。

### `MiRC003.GESTURES` — 手势名

```js
{ CLICK: "click", LONG: "long", DOUBLE: "double", REPEAT: "repeat" }
```

### `MiRC003.PHYSICAL_KEYS` — 遥控器物理按键

```js
[ { vk: 0x66, name: "电源键" }, { vk: 0x04, name: "语音键" }, ... ]
```

`vk` 即绑定中的 `source_vk`（内部规范化键码）。

#### 遥控器按键位置参考

遥控器正视布局如下，第三方可据此自行绘制遥控器 UI：

| 左列 | 中列 | 右列 |
| :--- | :--- | :--- |
| 电源键（小号圆形） |  | 语音键（小号圆形） |
|  | 上键（上侧四分之一圆环） |  |
| 左键（左侧四分之一圆环） | 确认键（圆环内分隔开大号圆形） | 右键（右侧四分之一圆环） |
|  | 下键（下侧四分之一圆环） |  |
| 返回键（中号圆形） |  | 音量加（竖胶囊型上部中号圆形） |
| 主页键（中号圆形） |  | 音量减（竖胶囊型下部中号圆形） |
| 菜单键（中号圆形） |  | TV 键（中号圆形） |

各按键对应的规范化键码（`PHYSICAL_KEYS` 中的 `name` / `vk`）：

| 位置 | 形状 | `PHYSICAL_KEYS` 名称 | `vk` |
| :--- | :--- | :--- | :--- |
| 左上 | 小号圆形 | 电源键 | `0x66` |
| 右上 | 小号圆形 | 语音键 | `0x04` |
| 方向环上 | 上侧四分之一圆环 | 方向上 | `0x52` |
| 方向环下 | 下侧四分之一圆环 | 方向下 | `0x51` |
| 方向环左 | 左侧四分之一圆环 | 方向左 | `0x50` |
| 方向环右 | 右侧四分之一圆环 | 方向右 | `0x4f` |
| 方向环中心 | 圆环内分隔开的大号圆形 | 确定键 | `0x28` |
| 左列第 1 行 | 中号圆形 | 返回键 | `0xf1` |
| 左列第 2 行 | 中号圆形 | 主页键 | `0x24` |
| 左列第 3 行 | 中号圆形 | 菜单键 | `0x5d` |
| 右列第 1 行 | 竖胶囊型上部中号圆形 | 音量+ | `0x80` |
| 右列第 2 行 | 竖胶囊型下部中号圆形 | 音量- | `0x81` |
| 右列第 3 行 | 中号圆形 | 电视键 | `0xc0` |

### `MiRC003.KEY_SLOT_COUNT` / `MiRC003.DEFAULT_KEY_NAMES` / `MiRC003.KEY_NAME_MAX_BYTES`

```js
MiRC003.KEY_SLOT_COUNT      // 16 — 物理槽位数量（== PHYSICAL_KEYS.length）
MiRC003.DEFAULT_KEY_NAMES   // ["电源键", "语音键 / 助手", ...] — 16 个出厂名称快照
MiRC003.KEY_NAME_MAX_BYTES  // 31 — 自定义名称的 UTF-8 字节上限
MiRC003.truncateKeyName(s)  // 按字符边界把 s 截断到 31 字节
```

**数组下标就是物理槽位号**，这一点是硬性约定，两个遥控器共用同一套槽位。但**不要再把数组下标
当成遥控器上报的按钮索引**：索引只在**同一条 HID 报告内**唯一，Google TV Remote (ZTKA-IR57)
有主 `0x0021`、副 `0x001D` 两条报告，各自从 1 开始编号（主报告索引 1 = 方向上，副报告索引 1 =
音量+），因此扁平化的「索引 N → 槽位 N-1」恒等映射对这台遥控器不成立。固件改为按
`(报告, 索引)` 查表得到规范键码（`MI_KEY_UP`、`MI_KEY_VOL_UP`…），再由键码定位到槽位并发出
`PHYSICAL_KEYS[slot].vk`。所以**槽位与规范键码才是稳定身份**：显示名称与发出的动作都由用户决定，
固件不再猜某个按钮「是什么键」，也不再假设索引与槽位有固定偏移。

`DEFAULT_KEY_NAMES` 是**加载时的快照**，不会被自定义名称污染，可用于「恢复默认名称」。
不要改用它去读 `PHYSICAL_KEYS[i].name` —— 界面会把用户名称写进那个字段。

> `name` 是**字节**上限而不是字符数：一个汉字占 3 字节，因此最多约 10 个汉字。
> 固件按裸字节截断（可能切断一个多字节字符），`setKeyName()` 会先在字符边界截断，
> 保证界面显示的内容与设备保存的内容完全一致。

### `MiRC003.SWITCH_MAP_DEFAULT` — 默认配置切换映射

```js
[
  { source_vk: 0x52, layer: 1 }, // 方向上 -> 配置 1
  { source_vk: 0x4f, layer: 2 }, // 方向右 -> 配置 2
  { source_vk: 0x51, layer: 3 }, // 方向下 -> 配置 3
  { source_vk: 0x50, layer: 4 }, // 方向左 -> 配置 4
]
```

对应 Keymap JSON 的顶层 `switch_map` 字段；详见[第 5 节](#5-keymap-json-结构)。

> `MiRC003.SWITCH_MAP_LOCKED`（`{ source_vk: 0x28, layer: 0 }`）表示「确定键」被固件锁定为
> 默认配置，不可修改，也不应写入 `switch_map`。

### `MiRC003.MOD_BITS` — 键盘修饰键位

```js
[ [0x01,"左Ctrl"], [0x02,"左Shift"], [0x04,"左Alt"], [0x08,"左Win"],
  [0x10,"右Ctrl"], [0x20,"右Shift"], [0x40,"右Alt"], [0x80,"右Win"] ]
```

用于键盘类动作的 `*_mod`。

### `MiRC003.MOUSE_BUTTONS` — 鼠标按键位

```js
[ [0x01,"左键"], [0x02,"右键"], [0x04,"中键"], [0x08,"后退键"], [0x10,"前进键"] ]
```

用于鼠标按键类动作的 `*_key`（按钮位掩码）。

### `MiRC003.HID_GROUPS` / `MiRC003.HID_EXTRA_GROUPS` / `MiRC003.CONSUMER_GROUPS`

键码分组，格式 `[组名, [[usage, 名称], ...]]`，可直接用于生成 `<optgroup>/<option>`：

```js
MiRC003.HID_GROUPS        // 屏幕键盘上的键：字母/数字/常用/符号/功能键/导航/小键盘
MiRC003.HID_EXTRA_GROUPS  // 屏幕键盘上没有、但 Windows 支持的常用键：
                          //   F13-F24、常用编辑/应用键（撤销/剪切/复制/粘贴/查找/静音/音量）
MiRC003.CONSUMER_GROUPS   // 多媒体：媒体/音量/系统/浏览器
```

> 默认界面按键编辑器里的「扩展按键」下拉框数据即来自 `HID_EXTRA_GROUPS`。

---

## 3. `MiRC003` 客户端

### 构造

```js
const dev = new MiRC003(options);
```

| 选项 | 类型 | 默认 | 说明 |
| :--- | :--- | :--- | :--- |
| `vendorId` | `number` | `0x303A` | 匹配的 USB VID |
| `productId` | `number` | `0x8304` | 匹配的 USB PID |
| `saveChunk` | `number` | `64` | 保存时每个 `KEYMAP_DATA` 分块字节数 |

### 连接与生命周期

| 方法 | 返回 | 说明 |
| :--- | :--- | :--- |
| `connect()` | `Promise<MiRC003>` | 弹出选择框、打开设备并占用 WebUSB 厂商接口 |
| `disconnect()` | `Promise<void>` | 关闭设备（未连接时为空操作） |
| `isConnected()` | `boolean` | 是否已连接并占用接口 |

### 事件

```js
dev.on("connect",    (info) => {});  // info = { outEndpoint, inEndpoint }
dev.on("disconnect", ()     => {});
dev.on("error",      (err)  => {});
dev.off("disconnect", handler);
```

> 设备被拔出时触发 `disconnect`。`on()` 返回 `this`，可链式调用。

### 底层传输

```js
dev.send(cmd, payloadObj?, rawBytes?)
```

| 参数 | 说明 |
| :--- | :--- |
| `cmd` | 命令字，见 `MiRC003.CMD` |
| `payloadObj` | 可选，会被 `JSON.stringify` 后作为负载 |
| `rawBytes` | 可选，`Uint8Array`，作为原始负载（优先级高于 `payloadObj`，如 `KEYMAP_DATA`） |

- 所有调用**自动串行化**：同一时刻只有一个请求在途；
- 返回 `Promise<object>`（解析后的 JSON 响应）；
- `status != 0` 时 reject，错误信息为 `设备返回错误 (status=N)`。

### 设备 / 状态

| 方法 | 返回 |
| :--- | :--- |
| `deviceInfo()` | `Promise<{ name, version, build, hardware, protocol, capabilities[] }>` |
| `status()` | `Promise<{ firmware, version, build, uptime_sec, ble_state, active_layer, battery, frames_decoded, samples_pushed, free_heap, free_psram, usb_mounted, switch_mode, config_rev, layout, reports_seen }>` |
| `telemetry()` | `Promise<{ source_vk, is_pressed, pressed_vk, duration_ms, action_type, modifier, key_code, consumer_code, active_layer, switch_mode }>` |

- `ble_state`：`0` 未连接 / `1` 扫描中 / `2` 连接中 / `3` 已连接 / `4` 语音中。
- `battery`：遥控器电量百分比（`0`~`100`），未知时为 `-1`。
- `telemetry().pressed_vk`：当前按下的物理键码，`0` 表示未按下。
- `switch_mode`：配置切换模式是否激活。
- `config_rev`：配置版本号，设备侧配置（层/绑定/切换映射）每次变更时递增；客户端可据此判断是否需要重新读取 keymap。
- `layout`：当前生效的 HID 报文方言，`"rc003"` / `"google_tv"` / `"unknown"`。
- `reports_seen`：开机以来解析过的 HID 通知条数。用于区分「遥控器根本没被听到」和「报文收到但没看懂」。

### 按键映射

| 方法 | 返回 | 说明 |
| :--- | :--- | :--- |
| `getKeymap()` | `Promise<Keymap>` | 读取完整多配置方案 |
| `saveKeymap(keymap)` | `Promise<object>` | 保存（内部走 `KEYMAP_BEGIN/DATA/COMMIT` 分块上传） |
| `resetKeymap()` | `Promise<object>` | 恢复出厂按键 |
| `setLayer(layer)` | `Promise<object>` | 切换设备当前配置（层索引 `0`-`4`） |

`Keymap` 结构见[第 5 节](#5-keymap-json-结构)。

### 按键名称

自定义名称按**物理槽位**存在设备 NVS 里，与按键映射相互独立：名称只影响显示，
按键实际发出什么由 [按键映射](#按键映射) 决定。

| 方法 | 返回 | 说明 |
| :--- | :--- | :--- |
| `keyNames()` | `Promise<string[]>` | 读取全部 16 个槽位的自定义名称 |
| `setKeyName(slot, name)` | `Promise<string[]>` | 写入一个槽位，返回写入后的完整名称表 |

```js
const names = await dev.keyNames();     // 长度恒为 16，下标 = 槽位
names[2];                               // "" 表示「未自定义，用出厂名称」

await dev.setKeyName(2, "我的上键");     // 自定义
await dev.setKeyName(2, "");            // 清空 -> 回到出厂名称
```

规则：

- 返回值**恒为 16 个字符串**，下标即槽位；`""` 表示使用出厂名称。
  `keyNames()` 对缺失 / 长度不足的响应做了归一化，因此老固件不会让页面抛异常。
- `slot` 必须是 `0`~`15` 的整数，否则**本地 reject**（`按键槽位超出范围（0-15）`），
  不会发出请求。校验作用于原始参数而非强制转换后的数字，
  否则 `null` / `""` 会静默落到槽位 0 —— 那是一次写到错误按键的真实写入。
- `name` 发送前按**字符边界**截断到 [`KEY_NAME_MAX_BYTES`](#mirc003key_slot_count--mirc003default_key_names--mirc003key_name_max_bytes) 字节。
- 固件对越界槽位返回 `{"error":"bad_slot"}` 且 `status = 2`，此时 Promise reject。

> 老固件没有 `0x27` / `0x28`，`keyNames()` / `setKeyName()` 会 reject。
> 名称属于锦上添花的功能，调用方应当 try/catch 后回退出厂名称，而不是让整个连接流程失败。

### 蓝牙

| 方法 | 参数 | 返回 |
| :--- | :--- | :--- |
| `bleScan()` | — | `Promise<{ devices: [{ name, mac, rssi, type }] }>` |
| `bleConnect(target)` | `{ mac, type, name }` | `Promise<object>` |
| `bleUnpair()` | — | `Promise<object>` |
| `bleInfo()` | — | `Promise<{ connected, state, name, mac, bound_mac, bound_name, battery }>` |
| `bleReconnect()` | — | `Promise<object>` |
| `bleRawReport(on)` | `on` 缺省为 `true` | `Promise<{ raw_report: bool }>` |
| `bleLayout(layout)` | `"auto"` / `"rc003"` / `"google_tv"` | `Promise<{ layout: string }>` |

- `bleRawReport(true)`：把接下来若干条 HID 通知与 ATVV 控制帧原样打成十六进制日志。
  用于弄不清某个按键上报什么时先打开它，再按那个键，然后到「运行日志」里看。
- `bleLayout()`：两个遥控器把不同的负载复用在同一特征上，字节本身有歧义，因此固件默认
  按广播名判断；此方法用于强制指定方言。

### 日志 / 系统

| 方法 | 返回 | 说明 |
| :--- | :--- | :--- |
| `getLogs()` | `Promise<{ logs: string[] }>` | 最近日志 |
| `clearLogs()` | `Promise<object>` | 清空日志 |
| `restart()` | `Promise<object>` | 重启设备 |
| `factoryReset()` | `Promise<object>` | 清空 NVS 并重启 |

---

## 4. `MiRC003.Keymap` 工具

纯函数集合，用于在内存中构建 / 检查 keymap 结构，避免第三方 UI 重复处理
`has_*`、`*_type` 等字段。所有函数不产生副作用（除 `setGesture` 等会修改传入对象外）。

### `action(type, opts?)`

构造动作对象。

```js
MiRC003.Keymap.action(MiRC003.ACTIONS.KEYBOARD_TAP, {
  modifier: 0,        // 修饰键位掩码
  keyCode: 0x28,      // HID 键码
  consumerCode: 0,    // 多媒体码
  targetLayer: 0,     // 目标配置（切换配置）
  dx: 0, dy: 0,       // 鼠标移动
  wheel: 0,           // 滚轮
  ms: 600,            // 长按/双击判定时间
  delayMs: 350,       // 连发起始延迟
  intervalMs: 70,     // 连发间隔
});
```

返回 `{ type, modifier, keyCode, consumerCode, targetLayer, dx, dy, wheel, ms, delayMs, intervalMs }`。

### 其他方法

| 方法 | 说明 |
| :--- | :--- |
| `clone(keymap)` | 深拷贝 keymap |
| `findLayer(keymap, id)` | 按 `id` 查找配置，返回 layer 或 `null` |
| `getBinding(layer, sourceVk)` | 读取某键的绑定，返回 binding 或 `null` |
| `ensureBinding(layer, sourceVk)` | 读取或新建绑定 |
| `removeBinding(layer, sourceVk)` | 删除某键的绑定，返回是否发生变化 |
| `getSwitchMap(keymap)` | 读取全局配置切换映射数组（缺省返回 `[]`） |
| `getSwitchTarget(keymap, sourceVk)` | 读取某键在切换映射中的目标配置，未映射返回 `-1` |
| `setSwitchTarget(keymap, sourceVk, layer)` | 设置某键的目标配置（`layer < 0` 表示移除）；返回条目或 `null` |
| `removeSwitchTarget(keymap, sourceVk)` | 从切换映射中移除某键，返回是否发生变化 |
| `getVoice(keymap)` | 读取全局语音设置 `{ trigger, hotkey_mode, max_sec }`，缺失字段按 `VOICE_DEFAULTS` 补齐 |
| `setVoice(keymap, settings)` | 合并写入 `keymap.voice`（只修改传入的字段），返回合并后的设置 |
| `setGesture(layer, sourceVk, gesture, action)` | 设置/清除手势；`action` 传 `null` 清除；返回 binding |
| `getGesture(layer, sourceVk, gesture)` | 以动作对象读回手势，未设置返回 `null` |
| `describe(layer, sourceVk)` | 生成可读的映射摘要 |
| `hidName(code)` | HID 键码对应名称（含 `HID_EXTRA_GROUPS`），`0` 返回 `"未选择"` |
| `validate(keymap)` | 结构校验，返回错误字符串数组（空数组 = 合法） |

### 完整示例

```js
const km = await dev.getKeymap();
const layer = MiRC003.Keymap.findLayer(km, 0);

// 单击：键盘 Enter
MiRC003.Keymap.setGesture(layer, 0x28, "click",
  MiRC003.Keymap.action(MiRC003.ACTIONS.KEYBOARD_TAP, { keyCode: 0x28 }));

// 长按：多媒体 音量+
MiRC003.Keymap.setGesture(layer, 0x28, "long",
  MiRC003.Keymap.action(MiRC003.ACTIONS.CONSUMER_TAP, { consumerCode: 0xe9, ms: 600 }));

// 双击：F13（扩展键，Windows 支持）
MiRC003.Keymap.setGesture(layer, 0x28, "double",
  MiRC003.Keymap.action(MiRC003.ACTIONS.KEYBOARD_TAP, { keyCode: 0x68 }));

// 连发：音量+
MiRC003.Keymap.setGesture(layer, 0x28, "repeat",
  MiRC003.Keymap.action(MiRC003.ACTIONS.CONSUMER_TAP,
    { consumerCode: 0xe9, delayMs: 350, intervalMs: 70 }));

// 语音键（0x04）：录音并发送微信语音快捷键（右Alt + ,）
MiRC003.Keymap.setGesture(layer, 0x04, "click",
  MiRC003.Keymap.action(MiRC003.ACTIONS.VOICE, { modifier: 0x40, keyCode: 0x36 }));

// 语音设置（全局）：按一下开始/再按一下结束，快捷键开始/结束各单击一次，最长 60 秒
MiRC003.Keymap.setVoice(km, {
  trigger: MiRC003.VOICE_TRIGGER.TOGGLE,
  hotkey_mode: MiRC003.VOICE_HOTKEY_MODE.TAP_BOTH,
  max_sec: 60,
});

// 清除长按
MiRC003.Keymap.setGesture(layer, 0x28, "long", null);

console.log(MiRC003.Keymap.describe(layer, 0x28));
console.log(MiRC003.Keymap.validate(km));   // []
await dev.saveKeymap(km);
```

---

## 5. Keymap JSON 结构

```jsonc
{
  "active_layer": 0,
  "switch_map": [
    { "source_vk": 82, "layer": 1 },   // 方向上 -> 配置 1
    { "source_vk": 79, "layer": 2 },   // 方向右 -> 配置 2
    { "source_vk": 81, "layer": 3 },   // 方向下 -> 配置 3
    { "source_vk": 80, "layer": 4 }    // 方向左 -> 配置 4
  ],
  "voice": {
    "trigger": 0,       // 0=按一下开始/再按结束 1=按住说话
    "hotkey_mode": 0,   // 0=全程按住 1=开始/结束各单击 2=仅开始单击
    "max_sec": 120      // 单次最长秒数，0=不限
  },
  "layers": [
    {
      "id": 0,                 // 0..4
      "name": "默认配置",
      "type": 0,               // 0=永久 1=一次性 2=超时
      "timeout": 15,           // type=2 时的超时秒数
      "color": "0x00FF00",     // 该配置指示灯颜色 RGB
      "bindings": [
        {
          "source_vk": 40,     // 物理键码（见 PHYSICAL_KEYS）
          "has_click": true,
          "click_type": 1,     // 动作类型，见 ACTIONS
          "click_mod": 64,     // 修饰键位（键盘动作）
          "click_key": 54,     // HID 键码（键盘动作）
          "click_cons": 233,   // 多媒体码（多媒体动作）
          "click_layer": 2,    // 目标配置（切换配置动作）
          "has_long": true, "long_ms": 600, "long_type": 4, "long_cons": 205,
          "has_double": false, "double_ms": 250, "double_type": 1, "double_key": 44,
          "has_repeat": true, "repeat_type": 4, "repeat_cons": 233,
          "repeat_delay_ms": 350, "repeat_interval_ms": 70
        }
      ]
    }
  ]
}
```

字段说明：

- `has_click` / `has_long` / `has_double`：是否启用对应手势。
- 动作字段前缀：`click_` / `long_` / `double_` / `repeat_`，后缀含义：
  - `_type`：动作类型（见 `ACTIONS`）。
  - `_mod` / `_key`：键盘修饰键与 HID 键码（键盘类动作）。
  - `_cons`：USB Consumer 多媒体码（多媒体类动作）。
  - `_layer`：目标配置（切换配置动作）。
  - `_key`：鼠标按键位掩码（鼠标按键类动作，见 `MOUSE_BUTTONS`）。
  - `_dx` / `_dy`：相对移动量（鼠标移动动作，`-127`~`127`；UI 以「方向 + 速度」配置，按住时持续移动，松开停止）。
  - `_wheel`：滚轮量（鼠标滚轮动作，`-127`~`127`，正数向上）。
  - `_ms`：长按 / 双击判定时间（仅 long/double）。
- `has_repeat`：连发（音量键常用）；`repeat_*` 同前缀规则。
- `type=10`（穿透继承）：该动作沿用默认配置的设置。
- 各配置方案相互独立：只有在该配置里设置过的按键才会生效；未设置的手势不会继承默认配置，除非显式设为 `type=10`。
- `switch_map`：全局「配置切换模式」映射，数组项为 `{ source_vk, layer }`。进入切换模式后按下
  `source_vk` 即切换到 `layer`；同一个 `source_vk` 只应出现一次，多个按键可指向同一配置。
  缺省值见 `MiRC003.SWITCH_MAP_DEFAULT`（上/右/下/左→1/2/3/4）。「确定键」由固件锁定为默认配置，
  无需也不应写入本表。
- `voice`：全局语音设置，对所有配置方案生效，字段见[语音设置常量](#语音设置常量keymapvoice)。
  可省略（固件保持原值）；也可以只发送 `{ "voice": {...} }` 而不带 `layers`，仅更新语音设置。
  旧固件不返回该字段时，`Keymap.getVoice()` 按 `VOICE_DEFAULTS` 补齐。

> 推荐使用 [`MiRC003.Keymap`](#4-mirc003keymap-工具) 读写上述字段，避免手写出错。

---

## 6. WebUSB 线协议

库已封装，如需自行实现可参考：

**请求帧（主机 → 设备）**

```text
0     1     2     3        4-5          6..N
'M'   'R'   cmd   rsvd     len (LE16)   payload (UTF-8 / JSON)
```

**响应帧（设备 → 主机）**

```text
0     1     2     3        4-5          6..N
'M'   'R'   cmd   status   len (LE16)   payload
```

- `status == 0` 表示成功。
- 厂商接口：USB class `0xFF`，BULK OUT / BULK IN 端点由浏览器自动识别。
- 大负载（如保存按键）建议分块发送（库已用 `KEYMAP_BEGIN/DATA/COMMIT` 实现）。

`payload` 是 UTF-8 的 JSON（`KEYMAP_DATA` 为裸字节）。按键名称两个命令的负载：

| 命令 | 请求 payload | 成功响应 payload |
| :--- | :--- | :--- |
| `KEY_NAMES_GET` `0x27` | 空 | `{"names":["","",...]}` —— 恒为 16 项，下标 = 槽位 |
| `KEY_NAMES_SET` `0x28` | `{"slot":0,"name":"电源"}` | 同 GET 的完整名称表 |
| `KEY_NAMES_SET`（槽位越界） | `{"slot":99,"name":"x"}` | `{"error":"bad_slot"}`，`status = 2` |

---

## 7. 错误处理

所有异步方法失败时 reject，错误对象为 `Error`：

```js
try {
  await dev.connect();
} catch (e) {
  if (e.name === "NotFoundError") {
    // 用户取消了设备选择
  } else {
    console.error(e.message);
  }
}
```

| 场景 | 错误信息 |
| :--- | :--- |
| 用户取消选择 | `NotFoundError: No device selected` |
| 设备返回非 0 状态 | `设备返回错误 (status=N)` |
| 未连接时调用 | `设备未连接` |
| 浏览器不支持 | `当前浏览器不支持 WebUSB` |
| 未找到厂商接口 | `未找到 WebUSB 厂商接口` |
| `setKeyName()` 槽位越界 | `按键槽位超出范围（0-15）`（本地 reject，不发请求） |

---

## 8. 兼容性

- 桌面版 Chrome / Edge（Windows / macOS / Linux）。
- 页面必须为安全上下文（HTTPS 或 `http://localhost`）。
- Windows 10/11 自动为厂商接口加载 WinUSB，无需 Zadig。
- Linux 需配置 udev 规则。

---

## 9. 维护约定

修改 `assets/mi-rc003.js` 的公开 API 时，请同步：

1. 更新本文件 `api.md` 中对应的常量表 / 方法表 / 示例。
2. 更新 [`README.md`](./README.md) 中的功能与引用（如有变化）。
3. 运行语法检查：`node --check assets/mi-rc003.js`。

> 版本号由维护者按发布流程统一管理，请勿在此处自行递增。

命令字与响应字段必须与固件保持一致：

| 库 | 固件 |
| :--- | :--- |
| `MiRC003.CMD` | `main/webusb/webusb_protocol.h` 的 `CMD_*` |
| `MiRC003.STATUS` | 同文件的 `WEBUSB_*` |
| `MiRC003.ACTIONS` | `main/keymap/key_state_machine.h` 的 `ACTION_*` |
| `MiRC003.VOICE_TRIGGER` / `VOICE_HOTKEY_MODE` / `VOICE_MAX_SEC_LIMIT` | 同文件的 `voice_trigger_t` / `voice_hotkey_mode_t` / `VOICE_MAX_SEC_LIMIT` |
| 遥测字段 | `key_telemetry_to_json()`（`main/keymap/key_config_storage.cpp`） |
| `MiRC003.PHYSICAL_KEYS` / `KEY_SLOT_COUNT` / `DEFAULT_KEY_NAMES` | 固件的规范化键码表与槽位顺序：**数组下标必须等于物理槽位号**，两个 Google TV 索引表都按它取值 |
| `MiRC003.KEY_NAME_MAX_BYTES` | 固件 `KEY_NAMES_SET` 的 31 字节上限 |

> `PHYSICAL_KEYS` 的顺序**不是**遥控器的物理布局，而是槽位编号：它由固件的规范化键码表与槽位顺序
> 决定，**不是**遥控器上报索引的偏移结果 —— 索引只在单条 HID 报告内唯一，双报告遥控器上会重号
> （见上一节）。型号不同、按键与索引的对应不同的遥控器，出厂名称可能对不上实物 ——
> 这是预期行为，正确做法是在「按键学习」里给槽位改名，而不是调整这个数组的顺序。
> 需要核对原始索引时，到「运行日志」页打开 `raw report`，看固件日志
> `HOGP: button chr#N idx=M -> key 0xXX (name) DOWN`。
