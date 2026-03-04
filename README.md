RTSP push-pull streaming test is the learning code for the RTSP push-pull streaming process


# RTSP 推流端和拉流端使用说明

## 概述

本目录包含两个完整的 RTSP 客户端程序：

1. **rtsp_pusher.c** - RTSP 推流端，从 MP4 文件读取数据并推送到 RTSP 服务器
2. **rtsp_client.c** - RTSP 拉流端，从 RTSP 服务器拉取流并接收 RTP 数据

## 编译方法

### 方法一：使用编译脚本（推荐）

```bash
chmod +x build_rtsp.sh
./build_rtsp.sh
```

### 方法二：手动编译

#### 编译推流端
```bash
gcc -o rtsp_pusher rtsp_pusher.c -lavformat -lavcodec -lavutil -lswscale -lpthread
```

#### 编译拉流端
```bash
gcc -o rtsp_client rtsp_client.c -lpthread
```

## 使用方法

### RTSP 推流端

#### 语法
```bash
./rtsp_pusher <mp4_file> <rtsp_url>
```

#### 参数说明
- `mp4_file` - MP4 文件路径（必需）
- `rtsp_url` - RTSP 推流地址（必需）

#### 示例
```bash
./rtsp_pusher test.mp4 rtsp://192.168.2.113:7554/app/live
```

#### 功能特点
- 支持从 MP4 文件读取视频和音频数据
- 自动检测视频和音频流
- 使用 FFmpeg 库进行媒体处理
- 支持完整的 RTSP 推流信令交互
- 支持文件循环播放（到达末尾自动回到开头）
- 使用 PTS 时间戳同步，确保音视频同步
- 支持 H264 视频和 AAC 音频推流

### RTSP 拉流端

#### 语法
```bash
./rtsp_client <rtsp_url> [duration]
```

#### 参数说明
- `rtsp_url` - RTSP 拉流地址（必需）
- `duration` - 拉流时长，单位秒（可选，默认 30 秒）

#### 示例
```bash
# 使用默认时长（30秒）
./rtsp_client rtsp://192.168.2.113:7554/app/live

# 指定时长（60秒）
./rtsp_client rtsp://192.168.2.113:7554/app/live 60
```

#### 功能特点
- 支持完整的 RTSP 拉流信令交互
- 自动检测视频和音频轨道
- 接收并解析 RTP 数据包
- 显示 RTP 包的详细信息（版本、序列号、时间戳等）
- 统计接收的数据包数量
- 支持 TCP Interleaved 模式传输

## RTSP 信令流程详解

### 推流端信令流程

```
┌─────────────┐     OPTIONS      ┌─────────────┐
│             │ ───────────────> │             │
│             │ <─────────────── │             │
│   推流端    │     200 OK       │   服务器    │
│             │                  │             │
│             │     ANNOUNCE     │             │
│             │ ───────────────> │             │
│             │ <─────────────── │             │
│             │     200 OK       │             │
│             │                  │             │
│             │     SETUP        │             │
│             │ ───────────────> │             │
│             │ <─────────────── │             │
│             │     200 OK       │             │
│             │                  │             │
│             │     RECORD       │             │
│             │ ───────────────> │             │
│             │ <─────────────── │             │
│             │     200 OK       │             │
│             │                  │             │
│             │     RTP Data     │             │
│             │ ==============>  │             │
│             │                  │             │
│             │     TEARDOWN     │             │
│             │ ───────────────> │             │
│             │ <─────────────── │             │
│             │     200 OK       │             │
└─────────────┘                  └─────────────┘
```

#### 1. OPTIONS
**作用**：查询服务器支持的 RTSP 方法

**请求格式**：
```
OPTIONS rtsp://server/stream RTSP/1.0
CSeq: 1
User-Agent: C-RTSP-Pusher

```

**预期响应**：
```
RTSP/1.0 200 OK
CSeq: 1
Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, ANNOUNCE, RECORD, SET_PARAMETER, GET_PARAMETER
```

#### 2. ANNOUNCE
**作用**：发送 SDP 描述媒体信息

**请求格式**：
```
ANNOUNCE rtsp://server/stream RTSP/1.0
CSeq: 2
User-Agent: C-RTSP-Pusher
Content-Type: application/sdp
Content-Length: <length>

v=0
o=- 0 0 IN IP4 127.0.0.1
s=TestStream
c=IN IP4 0.0.0.0
t=0 0
m=video 0 RTP/AVP 96
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1;profile-level-id=640020;sprop-parameter-sets=...
a=control:streamid=0
m=audio 0 RTP/AVP 97
a=rtpmap:97 MPEG4-GENERIC/44100/2
a=fmtp:97 profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3; config=...
a=control:streamid=1
```

**预期响应**：
```
RTSP/1.0 200 OK
CSeq: 2
Session: <session_id>
```

#### 3. SETUP
**作用**：建立 RTP 传输通道

**视频轨道请求**：
```
SETUP rtsp://server/stream/streamid=0 RTSP/1.0
CSeq: 3
User-Agent: C-RTSP-Pusher
Transport: RTP/AVP/TCP;unicast;interleaved=0-1

```

**音频轨道请求**：
```
SETUP rtsp://server/stream/streamid=1 RTSP/1.0
CSeq: 4
User-Agent: C-RTSP-Pusher
Transport: RTP/AVP/TCP;unicast;interleaved=2-3
Session: <session_id>

```

**预期响应**：
```
RTSP/1.0 200 OK
CSeq: 3
Session: <session_id>
Transport: RTP/AVP/TCP;unicast;interleaved=0-1
```

#### 4. RECORD
**作用**：通知服务器开始接收数据

**请求格式**：
```
RECORD rtsp://server/stream RTSP/1.0
CSeq: 5
User-Agent: C-RTSP-Pusher
Session: <session_id>
Range: npt=0.000-

```

**预期响应**：
```
RTSP/1.0 200 OK
CSeq: 5
Session: <session_id>
```

#### 5. TEARDOWN
**作用**：结束推流会话

**请求格式**：
```
TEARDOWN rtsp://server/stream RTSP/1.0
CSeq: 6
User-Agent: C-RTSP-Pusher
Session: <session_id>

```

**预期响应**：
```
RTSP/1.0 200 OK
CSeq: 6
Session: <session_id>
```

### 拉流端信令流程

```
┌─────────────┐     OPTIONS      ┌─────────────┐
│             │ ───────────────> │             │
│             │ <─────────────── │             │
│   拉流端    │     200 OK       │   服务器    │
│             │                  │             │
│             │     DESCRIBE     │             │
│             │ ───────────────> │             │
│             │ <─────────────── │             │
│             │     200 OK + SDP │             │
│             │                  │             │
│             │     SETUP        │             │
│             │ ───────────────> │             │
│             │ <─────────────── │             │
│             │     200 OK       │             │
│             │                  │             │
│             │     PLAY         │             │
│             │ ───────────────> │             │
│             │ <─────────────── │             │
│             │     200 OK       │             │
│             │                  │             │
│             │     RTP Data     │             │
│             │ <==============  │             │
│             │                  │             │
│             │     TEARDOWN     │             │
│             │ ───────────────> │             │
│             │ <─────────────── │             │
│             │     200 OK       │             │
└─────────────┘                  └─────────────┘
```

#### 1. OPTIONS
**作用**：查询服务器支持的 RTSP 方法

**请求格式**：
```
OPTIONS rtsp://server/stream RTSP/1.0
CSeq: 1
User-Agent: C-RTSP-Client

```

**预期响应**：
```
RTSP/1.0 200 OK
CSeq: 1
Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, etc.
```

#### 2. DESCRIBE
**作用**：获取 SDP 媒体描述

**请求格式**：
```
DESCRIBE rtsp://server/stream RTSP/1.0
CSeq: 2
User-Agent: C-RTSP-Client
Accept: application/sdp

```

**预期响应**：
```
RTSP/1.0 200 OK
CSeq: 2
Content-Type: application/sdp
Content-Length: <length>

v=0
o=- 0 0 IN IP4 127.0.0.1
s=TestStream
c=IN IP4 0.0.0.0
t=0 0
m=video 0 RTP/AVP 96
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1;...
a=control:streamid=0
m=audio 0 RTP/AVP 97
a=rtpmap:97 MPEG4-GENERIC/44100/2
a=fmtp:97 profile-level-id=1;...
a=control:streamid=1
```

#### 3. SETUP
**作用**：建立 RTP 传输通道

**视频轨道请求**：
```
SETUP rtsp://server/stream/streamid=0 RTSP/1.0
CSeq: 3
User-Agent: C-RTSP-Client
Transport: RTP/AVP/TCP;unicast;interleaved=0-1

```

**音频轨道请求**：
```
SETUP rtsp://server/stream/streamid=1 RTSP/1.0
CSeq: 4
User-Agent: C-RTSP-Client
Transport: RTP/AVP/TCP;unicast;interleaved=2-3
Session: <session_id>

```

**预期响应**：
```
RTSP/1.0 200 OK
CSeq: 3
Session: <session_id>
Transport: RTP/AVP/TCP;unicast;interleaved=0-1
```

#### 4. PLAY
**作用**：通知服务器开始发送数据

**请求格式**：
```
PLAY rtsp://server/stream RTSP/1.0
CSeq: 5
User-Agent: C-RTSP-Client
Session: <session_id>
Range: npt=0.000-

```

**预期响应**：
```
RTSP/1.0 200 OK
CSeq: 5
Session: <session_id>
RTP-Info: url=rtsp://server/stream/streamid=0,url=rtsp://server/stream/streamid=1
```

#### 5. TEARDOWN
**作用**：结束拉流会话

**请求格式**：
```
TEARDOWN rtsp://server/stream RTSP/1.0
CSeq: 6
User-Agent: C-RTSP-Client
Session: <session_id>

```

**预期响应**：
```
RTSP/1.0 200 OK
CSeq: 6
Session: <session_id>
```

## 测试流程

### 准备工作

1. 确保 ZLMediaKit 服务器正在运行
2. 准备一个测试用的 MP4 文件（test.mp4）
3. 确认网络连接正常

### 测试推流

```bash
# 启动推流端
./rtsp_pusher test.mp4 rtsp://192.168.2.113:7554/app/live
```

推流端会执行以下操作：
1. 连接到 RTSP 服务器
2. 发送 OPTIONS 请求
3. 发送 ANNOUNCE 请求（包含 SDP）
4. 为视频和音频轨道发送 SETUP 请求
5. 发送 RECORD 请求开始推流
6. 从 MP4 文件读取数据并通过 RTP 发送
7. 无限循环推流，直到手动终止

### 测试拉流

```bash
# 启动拉流端
./rtsp_client rtsp://192.168.2.113:7554/app/live 60
```

拉流端会执行以下操作：
1. 连接到 RTSP 服务器
2. 发送 OPTIONS 请求
3. 发送 DESCRIBE 请求获取 SDP
4. 为视频和音频轨道发送 SETUP 请求
5. 发送 PLAY 请求开始拉流
6. 接收 RTP 数据包并显示详细信息
7. 60 秒后发送 TEARDOWN 请求结束拉流

## 输出说明

### 推流端输出示例

```
MP4 文件: test.mp4
RTSP URL: rtsp://192.168.2.113:7554/app/live
SPS/PPS: Z2QAIKzZQLQW7ARAAAADAEAAAB4DxgxlgA==,aOviyyLA
解析 RTSP URL: IP=192.168.2.113, 端口=7554, 路径=/app/live
已连接到服务器 192.168.2.113:7554
发送 RTSP 请求:
OPTIONS rtsp://192.168.2.113:7554/app/live RTSP/1.0
...
接收 RTSP 响应:
RTSP/1.0 200 OK
...
推流线程启动 (TCP)
视频流: 720x720, 59 fps
音频流: 44100 Hz
开始推流...
```

### 拉流端输出示例

```
RTSP URL: rtsp://192.168.2.113:7554/app/live
拉流时长: 30 秒
已连接到服务器 192.168.2.113:7554
发送 RTSP 请求:
OPTIONS rtsp://192.168.2.113:7554/app/live RTSP/1.0
...
接收 RTSP 响应:
RTSP/1.0 200 OK
...
媒体信息: 视频=1 音频=1
Interleaved 通道: 视频=0 音频=2
RTP 接收线程启动 (TCP interleaved)
开始接收流...
[H264] SEQ=52245 TS=13075350 NALU=28 大小=1400
[AAC] SEQ=19964 TS=6302720 AU=0 大小=370
统计: 视频=16 音频=7
...
```

## 技术细节

### RTP 数据格式

#### TCP Interleaved 模式
```
+--------+---------+----------+----------------+
|  $ (1) | Channel |  Length  |  RTP Payload   |
|  0x24  | 1 byte  |  2 bytes |   N bytes      |
+--------+---------+----------+----------------+
```

#### RTP 头部格式
```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|X|  CC   |M|     PT      |       Sequence Number         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           Timestamp                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Synchronization Source (SSRC) identifier            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

#### 载荷类型（PT）
- **96**：H.264 视频
- **97**：AAC 音频

### 时间戳计算

#### 视频时间戳（H264）
- 时钟频率：90 kHz
- 计算公式：`timestamp = pts * 90000 / time_base`

#### 音频时间戳（AAC）
- 时钟频率：采样率（如 44100 Hz）
- 每帧采样点：1024
- 计算公式：`timestamp = pts * sample_rate / time_base`

### 线程模型

#### 推流端
- **主线程**：处理 RTSP 信令交互
- **推流线程**：单线程读取 MP4 文件，分别发送视频和音频 RTP 包

#### 拉流端
- **主线程**：处理 RTSP 信令交互
- **接收线程**：接收 RTP 数据包并解析

### 支持的媒体格式

#### 视频
- **编码**：H.264
- **RTP 封装**：RFC 6184
- **支持功能**：
  - AVCC 到 Annex B 格式转换
  - FU-A 分片传输（大包分片）
  - NALU 直接传输（小包）

#### 音频
- **编码**：AAC-LC
- **RTP 封装**：RFC 3640
- **支持功能**：
  - AU headers 封装
  - AudioSpecificConfig 解析

## 注意事项

### 依赖库

推流端需要以下库：
- FFmpeg (libavformat, libavcodec, libavutil, libswscale)
- pthread

拉流端需要以下库：
- pthread

### 安装 FFmpeg

Ubuntu/Debian:
```bash
sudo apt-get install ffmpeg libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
```

CentOS/RHEL:
```bash
sudo yum install ffmpeg-devel
```

### 网络配置

- 确保 ZLMediaKit 服务器可访问
- 检查防火墙设置，确保 RTSP 端口未被阻止
- 确保客户端有足够的权限建立 TCP 连接

### 常见问题

1. **连接失败**
   - 检查 ZLMediaKit 是否正在运行
   - 确认 IP 地址和端口是否正确
   - 检查网络连接

2. **编译错误**
   - 确保已安装 FFmpeg 开发库
   - 检查编译器版本是否支持 C99 标准

3. **运行时错误**
   - 检查 MP4 文件是否存在且格式正确
   - 确认 RTSP URL 格式正确
   - 检查是否有足够的系统资源

4. **音视频不同步**
   - 程序已使用 PTS 时间戳同步
   - 确保 MP4 文件的时间戳是正确的

## 扩展功能

### 推流端扩展

- 添加实时编码支持（从摄像头获取数据）
- 支持更多视频和音频编码格式
- 添加码率控制和自适应码率
- 支持多路推流

### 拉流端扩展

- 添加 RTP 数据保存功能
- 实现 RTP 数据包重组
- 添加媒体解码和播放功能
- 支持录制到文件

## 许可证

本程序基于 MIT 许可证发布。

## 联系方式

如有问题或建议，请联系开发者。
