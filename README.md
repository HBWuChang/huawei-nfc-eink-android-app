# Huawei NFC E-Ink Android App

Android 墨水屏刷新工具，当前版本 **v2.9.3-root**。通过 Shizuku（以 Root 或 Shell 运行）调用兼容系统的 NXP Q-Poll，再通过 Android ISO-DEP 向相同原厂配置的 **552×768 四色墨水屏**传图刷新。

## 本机 Root 适配（23127PN0CC / houji / SN220）

本分支针对 Android 16 + SN220 固件补齐 Root 全流程。实测 D1 只读、ISO-DEP 连接、D3 上传 476 包、D4 触发刷新均可到达本机屏幕；D4 后屏幕可能主动关闭 NFC 接口，日志会显示“刷新结果待确认”，不会重放 D4。

### Root 运行方式

本 ROM 的 SELinux 不允许应用直接执行 `/system/bin/su`，因此“Root”选项通过 **Shizuku（以 Root 运行）** 完成：

1. 在 Magisk 中允许本应用一次 `su`（手动授权即可）。
2. 选择“Root · 含 Q-Poll 兼容层”，点击任一操作。应用会用 `su` 自动启动 `moe.shizuku.privileged.api` 的 root 服务；首次可能需要再点一次完成 Shizuku 连接。
3. Shizuku 后端确认 UID=0 后，注入 HAL 兼容层，并以 UID 2000 + `u:r:shell:s0` 运行寻卡/APDU 进程；直接以 UID 0 调用 `setReaderMode` 会因本 ROM 的包名查询返回 null 而失败。

Shizuku 以 Shell（UID 2000）运行时仍可做普通 NFC-A 对照/芯片检查，但 Q-Poll 与读写会被阻止并提示改用 Root。

### Q-Poll 兼容层

兼容层是原始 C 代码，见 `native/qpoll_hook.c` / `native/qpoll_inject.c`，只修改 NFC HAL 进程内存里的两个 GOT 表项：

- 仅在 `/data/vendor/nfc/qphook/enable` 存在时，把 HAL 发出的 `RF_DISCOVER` NFC-A 技术字节 `00` 改为 `71`；
- 把 HAL 收到的 `61 03` / `61 05` 通知中的 `71` 映射回 `00`，供 Android NFA 正常建 Tag。

不写 NFC 固件、不写 DB/DA、不改系统文件。`enable` 仅在 Q-Poll 会话内存在，会话结束即删除，因此普通 NFC 不受影响。每个会话使用唯一的 `.so` 文件名注入，避免覆盖已映射的库导致 HAL 崩溃；旧文件加载后删除是安全的。兼容层本身留在 HAL 内存中，停用状态下为直通。

### 已知边界

- 默认寻卡改为 30 秒：屏幕休眠时 Q-Poll 唤醒可能超过 10 秒。D4 上电等待保持参考项目的 2000 ms，刷新前保场 0 s；D4 超时放宽到 60 s，ReaderMode presence 放宽到 60 s。
- D4 后固定保持 ReaderMode 射频场/持续供能 **30 秒**；DE 轮询窗口也是 30 秒。DE 查询遇到 TagLost 不再立即结束，而是继续保场到 30 秒，方便屏幕完成刷新。
- **关键协议差异**：参考 PN532 流程在 D1 后连续验证 PIN 两次。原 Android 实现只验证一次，D4 会 TagLost/不刷新；本分支改为 D8/PIN/D8 + 第二次 PIN，实测 D4 返回 `9000`，DE 返回忙状态。
- Q-Poll 能力测试为只读，不连接标签；测试后若后续寻不到标签，关闭再开启一次 NFC 即可清除本 ROM 的残留激活状态。
- D4 触发后标签丢失属于供能/屏幕状态现象，应用只报告“刷新结果待确认”，不重发刷新指令；请目视确认画面。
- 兼容层仅提供 `arm64-v8a`。`native/build.sh` 需要 NDK（已包含预编译 `assets/bin/arm64-v8a/`）。

## 功能

- 设备与测试：Shizuku（Root/Shell）授权、NFC 芯片配置识别、Q-Poll 能力测试、普通 NFC-A 对照。
- 图像与发送：选图、四色预览、旋转、裁切、黑白二值化、阈值、色块简化和抖动；支持全屏缩放与 1:1 像素查看。
- 预览和发送按钮位于页面上方，图片编辑选项位于下方；预览高度根据可用空间调整。
- 高级参数：上电等待时间、忙状态查询间隔；参数保存在本地。
- 保留屏幕规格和原厂扫描配置检查，不限制屏幕 UID；发送前确认，运行中可停止。

## 使用

1. 开启 NFC，保持手机解锁。已 Root 时在“设备与测试”页选择 Root；首次允许 Magisk 的 su 授权，应用会自动以 Root 启动 Shizuku，随后再点一次完成连接。仅用普通 NFC-A 时也可选择 Shizuku Shell 后端。
2. 点击“检查芯片 / NFC”，按需测试 Q-Poll。型号来自系统固件配置，能力以实际控制器响应和通信结果为准。
3. 在“图像与发送”页选择图片并调整预览，可先执行“只读检查屏幕”。
4. 点击“发送当前预览到墨水屏”，核对图片和参数后确认。持续贴合并保持应用在前台，直到操作结束。

仅适用于兼容 Q-Poll 的手机系统及相同原厂配置的目标屏；不能仅凭接口存在或型号名称判定兼容。刷新结果取决于实际供能与屏幕状态。中断可能留下不完整画面，程序不会自动重发刷新指令。

## 构建与验证

需要 Python 3、JDK 17、Android SDK platform android-34 和 build-tools 34.0.0。设置 `JAVA_HOME`、`ANDROID_HOME` 指向本机安装目录，然后在仓库根目录运行：

```sh
python3 tests/run_java.py
python3 build.py
```

如修改了原生兼容层，先运行 `ANDROID_NDK_HOME=... ./native/build.sh` 重新生成 `assets/bin/arm64-v8a/`，再运行 `python3 build.py`。

脚本使用 Android SDK 的命令行工具构建，无 Gradle、无构建期下载。Shizuku API / provider / aidl / shared 13.1.5 依赖已固定在 `libs/`。构建脚本当前使用 Unix 工具路径；Windows 可在 WSL 中运行。

如安装了 `lzokay==2.1.0`，可独立检查解压与扫描打包：

```sh
python3 tests/check_fixtures.py build/<protocol-tests-directory>
```

APK 和 SHA-256 输出在 `dist/`。首次构建会在 `build/` 生成本地调试签名密钥；密钥、构建缓存和 APK 不纳入 Git。自行构建时需要保留自己的密钥，以支持后续覆盖安装。

```sh
adb install -r dist/QPollLab-2.9.3-root.apk
```

## 隐私

本仓库仅包含 App 源码、测试、构建脚本、必要依赖与许可证。测试使用合成 UID，不包含原始手机日志、NFC 抓包或个人图片。

应用在运行时读取当前设备与标签信息，并在应用私有目录保存运行日志。日志可能包含设备型号、标签 UID 和本轮操作参数；分享日志前请自行检查。应用未申请网络权限。

## 参考逆向项目

本 App 的协议学习与实现参考了以下两个逆向仓库，感谢原作者公开研究成果：

- [kur0neko/huawei-epd-pn532](https://gitee.com/kur0neko/huawei-epd-pn532)：参考原厂图像扫描布局及 D3 传图、D4 刷新流程；对应代码遵循 PolyForm Noncommercial 1.0.0。
- [solar2ain/ink-nfc-fmsc-by-pn532](https://github.com/solar2ain/ink-nfc-fmsc-by-pn532)：参考公开的 D8/APDU 定义及 LZO1X literal-stream 格式；该项目采用 GPL-3.0-or-later。

具体参考提交、版权声明和许可说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 协议与许可

保留原厂驱动与扫描配置，无 DB/DA 配置写入。图像为 105984 字节，分块发送 D3，单次 D4 启动刷新，通过 DE 查询状态。

参考项目、第三方版权和许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。协议与扫描布局所参考代码使用 PolyForm Noncommercial 1.0.0，完整许可证及 Required Notice 保留在 `third-party/` 和 APK assets 中；请遵守其非商业条款。Shizuku 依赖遵循 MIT License。

## v2.9.3-root 说明

- Root 后端改为“su 启动 Shizuku Root + ShellBridge 注入 HAL 兼容层 + UID 2000/shell 收发 APDU”，解决本 ROM 直接以 UID 0 调 `setReaderMode` 的空包名异常。
- D1 校验接受本机 `C0=00000001` 前导（只校验长度与压缩标志），并显示固件 B2。
- 对齐参考 PN532 流程的两次 PIN 验证；这是 D4 成功返回 `9000` 的关键。
- 兼容层按会话启用 `RF_DISCOVER` 00→71 与入站 71→00；默认寻卡 30 秒。
- D4/D3 阶段的 TagLost 按“刷新结果待确认”处理，不重发 D4。

## v2.9.1 配置诊断

配置校验失败时，日志逐项显示不匹配字段、期望值和实际十六进制值；缺失或长度异常也显示对应字段。D1 只检查首字节时明确标注。仍执行原有兼容性检查，校验失败后不会认证、传图或刷新，不改写 DB/DA。UID 不参与兼容性校验。

## v2.9.2-test1 连接测试版（Issue #1）

针对寻到标签后 ISO-DEP 连接失败，增加最多三次连接尝试：仅 `IsoDep.connect()` 抛出 IOException、尚未发送任何 APDU 时重新启动 ReaderMode，获取同一 UID 的新 Tag；重新寻卡超时、取消或遇到不同 UID 即停止。协议通信开始后不自动重试，保留原有屏幕规格校验。

日志增加 CONNECT_ATTEMPT、ISO_DEP、CONNECTED / CONNECT_FAILED、APDU_BEGIN / APDU_FAILED 和异常堆栈；不记录 PIN 或图像内容。升级后重新连接 Shizuku，先执行“只读检查屏幕”，反馈从本轮开始到结束的完整日志及手机、系统、屏幕型号。此版本是连接恢复尝试与诊断增强，尚未在 Issue 中的目标屏实测，不代表已支持该型号。
