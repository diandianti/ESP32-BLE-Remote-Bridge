# AGENTS.md

本文件面向在此仓库工作的自动化代理 / 协作者，说明**编译、发布**与 **WebUSB API**
相关的硬性要求。修改代码前请先阅读。

## 1. 项目概况

- **固件**：ESP32-S3，ESP-IDF v6.x（当前 v6.1），C/C++。
- **功能**：BLE 直连小米遥控器 RC003，作为 USB 复合设备（UAC 麦克风 + HID 键盘/多媒体/鼠标
  + WebUSB 厂商接口）转发到 Windows。
- **配置站点**：`webusb-config/`，纯静态站点，浏览器 WebUSB 直连设备。
- **发布产物**：Windows 免安装烧录工具（`dist/`）+ 网页烧录固件（`webusb-config/flash/`）。

## 2. 编译环境

- 需要 **ESP-IDF v6.1**，芯片目标 `esp32s3`。
- 首次编译会通过 IDF Component Manager 自动拉取依赖
  （`espressif/esp_tinyusb`、`espressif/tinyusb`、`bblanchon/arduinojson`、`espressif/led_strip`）。
- 激活环境（Windows PowerShell，标准安装）：

  ```powershell
  . "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
  ```

- 若该 profile 不存在（EIM 安装布局），用 `export.ps1` 并显式指定路径：

  ```powershell
  $env:IDF_PATH           = "C:\esp\v6.1\esp-idf"
  $env:IDF_TOOLS_PATH     = "C:\Espressif"
  $env:IDF_PYTHON_ENV_PATH = "C:\Espressif\tools\python\v6.1\venv"
  . "C:\esp\v6.1\esp-idf\export.ps1"
  ```

  激活脚本要求 `$env:IDF_TOOLS_PATH\espidf.constraints.v6.1.txt` 存在；若缺失，可从
  `$env:IDF_TOOLS_PATH\tools\` 复制一份。

## 3. 编译要求

- 日常编译：

  ```powershell
  idf.py build
  ```

- 切换芯片目标（首次 / 更换硬件时）：

  ```powershell
  idf.py set-target esp32s3
  ```

- **不要提交**编译产物：`build/`、`sdkconfig`、`managed_components/`、`dist/`、`.cache/`
  已在 `.gitignore` 中忽略。
- 提交前确保 `idf.py build` 通过（应用分区余量充足，失败会以非零退出码结束）。

## 4. 发布要求

发布流程分三步，均使用仓库内脚本（不要在脚本之外手工拼装固件）：

1. **生成烧录固件**（编译 + 合并 bootloader/分区表/应用，默认覆盖全部板型）：

   ```bat
   build-firmware.bat
   build-firmware.bat -Profile n8r2      :: 仅生成指定板型（可逗号分隔）
   ```

   产物（按板型 `n16r8` / `n8r2` / `n4r2` 分开）：
   - `build/firmware/merged-flash-<板型>.bin` —— **Windows 免安装烧录工具用的固件**
     （默认板型另存一份 `build/merged-flash.bin` 以兼容旧流程）。
   - `webusb-config/flash/firmware/merged-flash-<板型>.bin`、`manifest-<板型>.json` 与
     `boards.json` —— 网页烧录固件与板型清单（**受版本控制**，发布时需一并提交）。

2. **打包 Windows 免安装烧录工具**（需先执行第 1 步）：

   ```bat
   package-release.bat
   package-release.bat -Version 1.2.1   :: 可显式指定版本
   ```

   产物在 `dist/`：`MI-RC003-Bridge-<版本>-win64/`、同名 `.zip`、`SHA256SUMS.txt`。

3. **发布 GitHub Release**：除上传 `dist/MI-RC003-Bridge-<版本>-win64.zip` 外，**必须一并上传
   全部原始固件**（按板型提供，便于用户单独烧录 / 回滚）：

   - `build/firmware/merged-flash-n16r8.bin`
   - `build/firmware/merged-flash-n8r2.bin`
   - `build/firmware/merged-flash-n4r2.bin`
   - `build/merged-flash.bin`（默认板型兼容文件）

   每个固件以原文件名作为 Release 附件，不要只发 zip 而遗漏原始固件。

### 版本号

- **唯一来源**：`main/version.h` 的 `FIRMWARE_VERSION`。发布前在此递增，例如 `"1.2.1"`。
- `build-firmware.ps1` / `package-release.ps1` 会自动读取该值并写入 `manifest.json`。
- 不要在任何其它文件里单独维护版本号；`api.md`、`app.js` 等处的版本由发布流程统一管理。

### WebUSB 着陆页

- `main/version.h` 的 `WEBUSB_LANDING_URL` / `WEBUSB_LANDING_SCHEME` 必须与
  `webusb-config/` 的部署地址一致（本 fork 生产：`diandianti.github.io/ESP32-BLE-Remote-Bridge/`，scheme=1）。
- 推送到 `main` 后 GitHub Pages 自动部署（`.github/workflows/static.yml`）。

## 5. `/dist` 烧录软件（Windows 免安装工具）

`dist/` 是**面向最终用户**的发布产物目录，由 `package-release.bat` 生成，**整个目录已被
`.gitignore` 忽略**，不要提交其中任何文件。

### 产物结构

```text
dist/
├── MI-RC003-Bridge-<版本>-win64/     # 免安装包目录
│   ├── flash.bat                     # 双击入口（调用同目录 flash.ps1）
│   ├── flash.ps1                     # 烧录逻辑：探测串口、等待下载模式、调用 esptool
│   ├── esptool.exe                   # 内置烧录工具（来自 espressif/esptool）
│   ├── LICENSE-esptool.txt           # esptool 原始许可证
│   ├── 使用说明.txt                  # 面向用户的说明（由 package-release.ps1 生成）
│   └── firmware/merged-flash-<板型>.bin # 各板型合并固件（含 bootloader + 分区表 + 应用）
├── MI-RC003-Bridge-<版本>-win64.zip  # 上述目录的压缩包，上传 GitHub Release
└── SHA256SUMS.txt                    # 发布 zip 的 SHA-256
```

### 工作原理（对用户）

- 用户**无需 Python / ESP-IDF**：双击 `flash.bat`，`flash.ps1` 先列出 `firmware/` 内可用板型
  供用户选择（`-Profile` 可跳过选择），再自动探测串口（优先 `VID_303A&PID_1001` 的 USB-JTAG
  串口），等待最多 **90 秒**进入下载模式，然后将 `firmware/merged-flash-<板型>.bin` 写入偏移
  `0x0`，成功后设备自动重启。
- 常用参数：`flash.bat -Profile n8r2`、`flash.bat -Port COM5`、`-Erase`（整片擦除）、
  `-Baud 460800`。
- 设备正运行固件时原生 USB 口不提供串口，需先按住 `BOOT` → 点按 `RST` → 松开 `BOOT`
  进入 ROM 下载模式（脚本会在无串口时打印该提示）。

### Agent 要求

- **唯一来源是脚本，不是 `dist/`**：烧录逻辑改动只改 `tools/standalone/flash.bat` /
  `tools/standalone/flash.ps1`；打包清单改动只改 `tools/package-release.ps1`。
  永远不要手工编辑或拼装 `dist/` 下的文件。
- **不要提交** `dist/` 内任何内容（含 `esptool.exe`、`*.zip`、`SHA256SUMS.txt`）；它们
  均由发布流程重新生成。发布时将 zip 上传 GitHub Release。
- **重新生成**：改动 standalone 脚本或固件后，依次执行
  `build-firmware.bat` → `package-release.bat`，确认 `dist/` 产物可正常生成。
- `flash.ps1` 必须兼容 esptool 新旧命令（`write-flash` 与 `write_flash`），并保持
  串口优先级、90 秒等待、下载模式提示等行为不回退。
- 用户可见文案（`使用说明.txt`、控制台提示）与固件版本号由脚本自动填充，不要手工写死
  版本号；版本以 `main/version.h` 为准。

## 6. WebUSB API 使用要求

浏览器端通信库为 `webusb-config/assets/mi-rc003.js`（挂载全局 `MiRC003`，无依赖）。

- **权威文档**：`webusb-config/api.md`。任何公开 API 变更必须同步更新该文件。
- **运行环境**：仅桌面版 Chrome / Edge；页面必须处于安全上下文（`https://` 或
  `http://localhost`）；移动端不支持。
- **调用方式**：所有请求经 `dev.send(cmd, payload)` 自动串行化；业务方法
  （`status()`、`getKeymap()`、`saveKeymap()`、`bleScan()` 等）返回 Promise。
- **Keymap 读写**：使用 `MiRC003.Keymap` 工具（`findLayer`、`setGesture`、`action`、
  `validate` 等）构造结构，避免手写 `has_*` / `*_type` 字段。保存走
  `KEYMAP_BEGIN/DATA/COMMIT` 分块上传。
- **常量必须与固件一致**（改一端必须同步另一端）：

  | 库 | 固件 |
  | :--- | :--- |
  | `MiRC003.CMD` | `main/webusb/webusb_protocol.h` 的 `CMD_*` |
  | `MiRC003.STATUS` | 同文件的 `WEBUSB_*` |
  | `MiRC003.ACTIONS` | `main/keymap/key_state_machine.h` 的 `ACTION_*` |
  | 遥测字段 | `key_telemetry_to_json()`（`main/keymap/key_config_storage.cpp`） |

- **修改库后必须**：
  1. 更新 `api.md`（常量表 / 方法表 / 示例）。
  2. 如功能或引用有变化，更新 `webusb-config/README.md` 与根 `README.md`。
  3. 运行语法检查：`node --check webusb-config/assets/mi-rc003.js`。

## 7. 代码约定

- 固件为 C/C++（ESP-IDF），浏览器端为原生 JS，均**不引入构建步骤**（站点直接引用源文件）。
- 跟随既有命名与文件组织；新增模块放入 `main/<域>/` 并在 `main/CMakeLists.txt` 登记。
- 除非被要求，不要添加注释、不要提交 `build/`、`dist/` 等生成物、不要自动修改版本号。

## 8. Windows 脚本与中文编码

- 运行环境为 **Windows PowerShell 5.1**，它按系统 ANSI 代码页（简体中文为 GBK/936）解析
  **无 BOM** 的脚本文件。用工具生成的 `.ps1` 通常是无 BOM 的 UTF-8，其中的中文字面量
  会被误读为 GBK，导致控制台输出或经 API 写入的内容出现**乱码**。
- 规避方式（二选一）：
  1. 含中文字面量的 `.ps1` 保存为 **UTF-8 with BOM**；或
  2. 脚本保持纯 ASCII，把中文放入单独的 UTF-8 数据文件，用
     `[System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)` 读取。
- 通过 GitHub API 写入中文（如 Release 说明）时，请求体必须用 UTF-8 字节发送：
  `[System.Text.Encoding]::UTF8.GetBytes($json)`，并设置
  `Content-Type: application/json; charset=utf-8`。
- 写入后应校验服务端返回文本与本地 UTF-8 源一致（例如比较 API 返回的 `body` 与源文件内容）。
