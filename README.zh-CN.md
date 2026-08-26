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
- 长按 A 呼出完全由眼睛组成的极简菜单；菜单内单击 A 浏览电量、亮度、音量和网络，双击 A 确认，B 返回。普通界面短按 A/B 均不切换表情；电量和网络继续用表情传达状态，亮度与音量直接由眼睛睁开幅度表达；
- 45 秒无操作后自动变暗并降低渲染频率，60 秒后清黑并关闭 AMOLED，触摸、按键或明显移动可唤醒；
- 亮度、休眠时长和安静时段保存在设备 NVS 中；
- RX8130 RTC 驱动夜间困倦状态，并支持通过串口校时；
- 串口语义命令为未来语音识别或外部控制提供稳定入口。

## 交互方式

| 操作 | 结果 |
| --- | --- |
| 单击 | `happy` |
| 双击 | `surprised` |
| 按住并移动 | 眼睛和头部持续跟随触点 |
| 长按 | `angry` |
| 左右滑动 | 根据滑动方向作出短暂好奇或困惑反应，随后恢复当前状态 |
| 向上 / 向下滑动 | `surprised` / `sleepy` |
| 缓慢倾斜设备 | 视线持续跟随倾斜方向 |
| 快速移动设备 | 眼睛和头部产生短暂惯性，但不改变当前表情 |
| 普通界面 A / B | 长按 A 呼出眼睛菜单；短按 A/B 均不执行表情切换 |
| 根菜单内 A / B | 单击 A 浏览项目，双击 A 确认，B 关闭菜单 |
| 电量页 | 查看百分比和由右眼开度表达的剩余电量；B 返回 |
| 亮度 / 音量页 | 单击 A 快速循环档位，B 返回；右眼开度同步表达当前档位 |
| 网络页 | 根菜单双击 A 只进入状态页；进入后重新长按 A 约两秒才开始配网，B 取消或返回；新网络成功前保留旧凭据 |
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

RTC 使用设备当地时间，不自动处理时区。`dim` 和 `screenoff` 的单位为秒；亮度、超时、安静时段和 Wi-Fi 凭据会持久化保存。`KK-XXXX` 中的 `XXXX` 是由本机芯片唯一 ID 派生出的末四位十六进制字符，只用于区分附近的多台 KK；本机为 `KK-8428`。配网页面仅支持 2.4 GHz 网络，最长开放五分钟，且不会把提交的密码输出到串口。候选网络只有连接成功后才替换旧凭据，失败时会自动恢复原网络。

## 仓库结构

| 路径 | 用途 |
| --- | --- |
| `src/avatar_engine.*` | 表情目录、时间轴、缓动、绘制和交互物理 |
| `src/main.cpp` | 设备初始化、触摸、IMU、按键、震动、诊断和串口命令 |
| `src/wifi_pairing.*` | 非阻塞联网、临时配网页面和凭据交接 |
| `docs/HARDWARE_BASELINE.md` | 硬件能力和验证边界 |
| `docs/ENGINEERING_NOTES.md` | 渲染实验、测量数据和实现决策 |
| `docs/ROADMAP.md` | 计划工作和暂不支持的能力 |

## 当前限制

- 麦克风和语音/大模型服务尚未接入；Wi-Fi 配网只是传输基础，串口命令仍用于模拟语义语音事件。
- 深度睡眠和外部扩展口尚未集成；当前节能策略只关闭 AMOLED，并保持输入采样以便快速唤醒。
- RTC 已接入，但仍需通过串口或未来的网络同步功能设置当地时间。
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
