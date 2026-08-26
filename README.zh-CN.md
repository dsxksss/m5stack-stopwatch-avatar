# KK — M5Stack StopWatch 表情角色

[English](README.md) | [简体中文](README.zh-CN.md)

[![构建固件](https://github.com/Trentct/m5stack-stopwatch-avatar/actions/workflows/build.yml/badge.svg)](https://github.com/Trentct/m5stack-stopwatch-avatar/actions/workflows/build.yml)
[![许可证：AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](LICENSE)

认识一下 **KK**：一个住在 M5Stack StopWatch 里的小表情角色。

KK 运行在 M5Stack StopWatch 的圆形 AMOLED 屏幕上。它的眼睛、眼皮、眉线、关键帧和过渡均由 C++ 实时绘制，不使用图片序列帧。产品画面保持纯黑背景，并针对 466 × 466 圆屏、局部刷新和直接交互进行了优化。

> 这是一个社区项目，与 M5Stack 官方没有隶属或背书关系。

## 主要特点

- 11 种程序化表情：`idle`、`listening`、`thinking`、`happy`、`excited`、`curious`、`confused`、`angry`、`surprised`、`sad`、`sleepy`；
- 通过 GPIO 38 的 CO5300 TE 信号进行硬件垂直同步，以约 60 fps 渲染，并使用动态脏矩形减少 AMOLED 传输开销；
- 支持单击、双击、长按、连续触摸跟随和上下/左右滑动；
- 加速度计与陀螺仪共同驱动倾斜跟随，眼睛先移动、头部稍后跟随，并受中心安全区限制；
- 快速移动设备会产生短暂惯性反馈，但不会切换当前表情；
- 表情由触摸、动作、电量、充电和网络等情境自动驱动，不提供手动表情目录；
- 表情、切换、电量状态、亮度和唤醒均配有原创的柔和 pop、boop、blip 合成音效，安静时段自动静音；
- 长按 A+B 进入硬件诊断；
- 长按 A 呼出完全由眼睛组成的极简菜单；菜单内单击 A 浏览亮度、音量、夜静、网络和固件版本，双击 A 确认，B 返回。普通界面短按 A/B 均不切换表情；向下滑动直接查看电量，亮度与音量由眼睛睁开幅度表达；
- 45 秒无操作后自动变暗并降低渲染频率，60 秒后清黑并关闭 AMOLED，触摸、按键或明显移动可唤醒；
- 亮度、休眠时长和安静时段保存在设备 NVS 中；
- Wi-Fi 连接后通过 NTP 自动同步中国标准时间并写入 RX8130 RTC，RTC 驱动夜间困倦和安静时段；
- 串口语义命令为未来语音识别或外部控制提供稳定入口。

## 交互方式

| 操作 | 结果 |
| --- | --- |
| 单击 | `happy` |
| 双击 | `surprised` |
| 按住并移动 | 眼睛和头部持续跟随触点 |
| 长按 | `angry` |
| 左右滑动 | 根据滑动方向作出短暂好奇或困惑反应，随后恢复当前状态 |
| 向上滑动 | `surprised` |
| 向下滑动 | 查看电量百分比和充电状态；片刻后自动恢复，B 可提前返回 |
| 缓慢倾斜设备 | 视线持续跟随倾斜方向 |
| 快速移动设备 | 眼睛和头部产生短暂惯性，但不改变当前表情 |
| 普通界面 A / B | 长按 A 呼出眼睛菜单；短按 A/B 均不执行表情切换 |
| 根菜单内 A / B | 单击 A 浏览项目，双击 A 确认，B 关闭菜单 |
| 亮度 / 音量页 | 单击 A 快速循环档位，B 返回；右眼开度同步表达当前档位 |
| 夜静页 | 默认关闭；单击 A 开关 `22:00–07:00` 定时静音并保存，B 返回；关闭静音不会关闭夜间困倦表情 |
| 网络页 | 默认只显示“网络 / 已连、未连或失败”；需要配网或换网时按住 A，眼睛才显示操作提示，约两秒后开始配网；B 取消或返回；新网络成功前保留旧凭据 |
| 版本页 | 只读显示固件版本 `0.6.2`，B 返回 |
| 长按 A+B | 进入 / 退出硬件诊断 |

`idle`、`listening` 和 `thinking` 是可持续停留的基础状态。其他反应播放完成后会返回触发前的基础状态，而不是固定回到待机。

## 硬件

- [M5Stack StopWatch Dev Kit（C152）](https://docs.m5stack.com/en/core/StopWatch)
- ESP32-S3R8、16 MB Flash、8 MB PSRAM
- 1.75 英寸 466 × 466 圆形 AMOLED 触摸屏
- BMI270 六轴惯性传感器
- CST820B 触摸控制器
- 两个可编程按键和内置震动马达

接口、地址和当前验证边界请参阅[硬件基础资料](docs/HARDWARE_BASELINE.md)。

## 编译

需要准备：

- [PlatformIO Core](https://platformio.org/) 6.1.18
- USB-C 数据线
- M5Stack StopWatch

已经验证的依赖提交均锁定在 [`platformio.ini`](platformio.ini) 中。

```sh
pio run
```

## 烧录与串口监视

通过 USB-C 连接 StopWatch。如果没有自动开始烧录，可以按住复位键约两秒，在绿色 LED 亮起时松开。

```sh
pio run --target upload
pio device monitor --baud 115200
```

串口监视器接受 `happy`、`thinking`、`sleepy` 等表情名称，也支持以下播放测试命令：

```text
once <expression>
loop <expression>
pingpong <expression>
sound
volume 0-160
```

日常伴侣功能还支持以下串口命令：

```text
status
time
time 2026-08-26 20:00:00
brightness 150
dim 60
screenoff 300
quiet 22 7
wifi
wifi pair
wifi retry
wifi forget
screen on
screen off
```

网络校时固定使用中国标准时间 `UTC+8`（`Asia/Shanghai`，无夏令时），联网后自动同步，失败一分钟后重试，成功后每六小时重写 RTC；串口仍可手动校时。`dim` 和 `screenoff` 的单位为秒；亮度、超时、夜间静音开关、安静时段和 Wi-Fi 凭据会持久化保存。`KK-XXXX` 中的 `XXXX` 是由本机芯片唯一 ID 派生出的末四位十六进制字符，只用于区分附近的多台 KK；本机为 `KK-8428`。配网页面仅支持 2.4 GHz 网络，最长开放五分钟，且不会把提交的密码输出到串口。候选网络只有连接成功后才替换旧凭据，失败时会自动恢复原网络。

## 仓库结构

| 路径 | 用途 |
| --- | --- |
| `src/avatar_engine.*` | 表情目录、时间轴、缓动、绘制和交互物理 |
| `src/main.cpp` | 设备初始化、触摸、IMU、按键、震动、诊断和串口命令 |
| `src/wifi_pairing.*` | 非阻塞联网、临时配网页面和凭据交接 |
| `docs/HARDWARE_BASELINE.md` | 硬件能力和验证边界 |
| `docs/ENGINEERING_NOTES.md` | 渲染实验、测量数据和实现决策 |
| `docs/ROADMAP.md` | 计划工作和暂不支持的能力 |

## 下一步执行交接：局域网短消息进入眼睛

该功能在固件 `0.6.2` 中尚未实现。目标是让同一 Wi-Fi 内的手机发送简短消息，并直接显示在 KK 的两只眼睛中，不在设备端增加传统面板。

### 必须实现的行为

- 在 STA 联网状态提供 `/message` 页面，包含 `left`、`right` 两个紧凑输入框，分别对应左右眼文字。
- 提供 `POST /api/message`，接收 URL 编码的 `left`、`right` 和可选 `hold_ms`；停留时间限制在 `1500–10000 ms`，默认 `3400 ms`。
- 每只眼最多四个可见 UTF-8 字符，整个请求体不超过 128 字节；缺失、格式错误或超长输入返回 HTTP `400` 或 `413`。
- HTTP 回调只负责校验并写入待处理消息，绘制、音效、震动和表情状态切换必须留在主循环，不能让网络请求阻塞 TE 同步动画。
- 复用 `AvatarEngine::setEyeMessage()` 以及现有渐显、文字呼吸、眨眼甩头退出和基础表情恢复逻辑；B 可提前关闭。
- 诊断、配网、眼睛菜单和电量状态优先；这些模式活动时保留一条待显示消息，新消息可覆盖旧的待显示消息。
- 接收消息可以唤醒 AMOLED 并刷新空闲计时，但不能永久手动选择表情。

### 网络与安全边界

- 只在 STA 已连接时提供消息页面；启动配网门户前必须停止消息服务，重新联网后再启动，同一时间只能有一个服务占用 80 端口。
- 串口不得输出 Wi-Fi 密码、消息正文或请求体；插入 HTML 的文字必须转义，接收的文字不得作为 HTML 解释。
- 第一阶段只支持局域网，不加入云中继、端口转发、鉴权、麦克风、语音识别或大模型。README 中要明确：加入本地网络的客户端在鉴权实现前都能提交消息。
- 建议新增小型 `local_message_server.*` 模块，由 `main.cpp` 显式协调启动和停止，不要在 `wifi_pairing.cpp` 的 HTTP 回调里直接绘制。

### 验收标准

1. 配网、旧网络恢复以及 B 取消门户仍正常。
2. 连续提交 20 条合法消息，不重启、不持续降帧，并能恢复提交前的基础表情。
3. 非法 UTF-8、空消息、超长字段和超大请求体会被拒绝且不显示。
4. 菜单或状态活动时消息延后，退出高优先级模式后显示最新待处理消息。
5. 真机加载页面并连续提交时仍接近 60 fps、TE 同步 100%，且没有持续帧超时。
6. 完成后更新中英文 README 与工程记录，写明最终接口、内存占用和真机测量结果。

## 当前限制

- 麦克风和语音/大模型服务尚未接入；Wi-Fi 配网只是传输基础，串口命令仍用于模拟语义语音事件。
- 深度睡眠和外部扩展口尚未集成；当前节能策略只关闭 AMOLED，并保持输入采样以便快速唤醒。
- 网络时区目前固定为中国标准时间；如在其他时区使用，仍需增加时区设置入口。
- 尚未完成长期续航测量，默认超时只是安全起点。
- 动作与手势的主观体验可能会随设备握持方式而变化。

## 灵感与来源

本项目的表情、动画和播放分层受到 [Bible Strong Avatar Lab](https://github.com/smontlouis/bible-strong-avatar-lab) 启发。它是针对 ESP32 硬件重新实现的独立 C++ 项目，不包含上游网页应用、TypeScript 源码、导出的角色数据或视觉素材。

两者的关系可以概括为：**受到架构启发，为完全不同的硬件重新实现。**

硬件初始化、引脚映射和 IMU 屏幕坐标处理参考了 M5Stack 官方的 [StopWatch User Demo](https://github.com/m5stack/M5StopWatch-UserDemo)。详情请参阅[第三方声明](THIRD_PARTY_NOTICES.md)。

## 参与贡献

欢迎提交 Issue 和 Pull Request。修改前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)，并运行 `pio run`。涉及硬件的结论应尽可能附上真机验证证据。

## 许可证

本项目使用 [GNU Affero General Public License v3.0 or later](LICENSE) 开源。
