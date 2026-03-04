/**
 * @file rtsp_client.c
 * @brief RTSP 拉流客户端 - 从 RTSP 服务器接收音视频流
 *
 * @功能说明
 *   本程序实现了一个完整的 RTSP 拉流客户端，支持：
 *   - H264 视频流接收和解析
 *   - AAC 音频流接收和解析
 *   - TCP 传输模式（RTP over RTSP interleaved）
 *   - 实时统计音视频包数量
 *
 * @支持的媒体格式
 *   - 视频: H264 RTP 封装 (RFC 6184)
 *   - 音频: AAC RTP 封装 (RFC 3640)
 *   - 传输: RTP/AVP/TCP (interleaved 模式)
 *
 * @编译方法
 *   gcc -o rtsp_client rtsp_client.c -lpthread
 *   或使用: ./build_rtsp.sh
 *
 * @使用方法
 *   ./rtsp_client <rtsp服务器地址> [拉流时长(秒)]
 *
 * @参数说明
 *   rtsp_url   - RTSP 流地址，例如: rtsp://192.168.2.113:7554/inrico/test_stream
 *   duration   - 拉流时长，单位秒（可选，默认 30 秒）
 *
 * @示例
 *   ./rtsp_client rtsp://192.168.2.113:7554/inrico/test_stream
 *   ./rtsp_client rtsp://192.168.2.113:7554/inrico/test_stream 60
 *
 * @RTSP 信令流程
 *   1. OPTIONS    - 查询服务器支持的方法
 *   2. DESCRIBE   - 获取 SDP 媒体描述
 *   3. SETUP      - 建立 RTP 传输通道
 *   4. PLAY       - 开始播放/接收流
 *   5. TEARDOWN   - 结束播放
 *
 * @RTP 解析功能
 *   - TCP Interleaved 模式: 接收 $ + channel + length + payload 格式
 *   - H264 解析: 识别 NALU 类型（1=关键帧, 28=FU-A 分片等）
 *   - AAC 解析: 解析 AU headers，提取 AU 大小和索引
 *
 * @输出信息
 *   - 每秒统计视频包和音频包数量
 *   - 显示每个 RTP 包的序列号、时间戳、负载类型
 *
 * @作者
 *   基于 RTSP/RTP 协议实现
 *
 * @日期
 *   2026-03-03
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>

/** 最大 RTP 负载大小 */
#define MAX_RTP_PAYLOAD 1500

// RTSP 客户端状态
typedef enum {
    RTSP_STATE_INIT,
    RTSP_STATE_OPTIONS,
    RTSP_STATE_DESCRIBE,
    RTSP_STATE_SETUP,
    RTSP_STATE_PLAY,
    RTSP_STATE_TEARDOWN,
    RTSP_STATE_FINISH
} RTSPState;

// RTSP 客户端结构
typedef struct {
    int socket;
    char session_id[128];
    char sdp[4096];
    int cseq;
    RTSPState state;
    char url[512];
    char server_ip[64];
    int server_port;
    char stream_path[256];
    pthread_t receive_thread;
    int running;
    int has_video;
    int has_audio;
    int video_interleaved;
    int audio_interleaved;
    FILE *video_file;
    FILE *audio_file;
} RTSPClient;

// 解析 RTSP URL
int parse_rtsp_url(const char *url, char *server_ip, int *server_port, char *stream_path) {
    char tmp_url[512];
    strcpy(tmp_url, url);
    
    char *host_start = strstr(tmp_url, "rtsp://");
    if (!host_start) {
        return -1;
    }
    host_start += 7;
    
    char *port_start = strchr(host_start, ':');
    char *path_start = strchr(host_start, '/');
    
    if (!path_start) {
        return -1;
    }
    
    if (port_start && port_start < path_start) {
        *port_start = '\0';
        strcpy(server_ip, host_start);
        *server_port = atoi(port_start + 1);
    } else {
        *path_start = '\0';
        strcpy(server_ip, host_start);
        *server_port = 554;
    }
    
    strcpy(stream_path, path_start);
    return 0;
}

// 发送 RTSP 请求
int send_rtsp_request(int socket, const char *request) {
    return send(socket, request, strlen(request), 0);
}

// 接收 RTSP 响应
int receive_rtsp_response(int socket, char *response, int max_len) {
    int total_received = 0;
    int content_length = -1;
    int header_received = 0;
    int header_size = 0;
    int timeout = 5000;
    struct timeval tv;
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    while (total_received < max_len - 1) {
        int bytes_received = recv(socket, response + total_received, max_len - total_received - 1, 0);
        if (bytes_received <= 0) {
            break;
        }
        total_received += bytes_received;
        response[total_received] = '\0';
        
        if (!header_received) {
            char *content_length_str = strstr(response, "Content-Length:");
            if (content_length_str) {
                content_length = atoi(content_length_str + 15);
            }
            
            if (strstr(response, "\r\n\r\n")) {
                header_received = 1;
                header_size = strstr(response, "\r\n\r\n") - response + 4;
                
                if (content_length < 0) {
                    break;
                }
            }
        } else {
            if (content_length >= 0 && total_received >= header_size + content_length) {
                break;
            }
        }
    }
    
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    return total_received;
}

// 从响应中提取 Session ID
void extract_session_id(const char *response, char *session_id) {
    const char *session_line = strstr(response, "Session: ");
    if (session_line) {
        session_line += 9;
        char *end = strchr(session_line, '\r');
        if (end) {
            strncpy(session_id, session_line, end - session_line);
            session_id[end - session_line] = '\0';
        }
    }
}

// 从 SDP 中提取媒体信息
void extract_media_info(const char *sdp, int *has_video, int *has_audio) {
    *has_video = (strstr(sdp, "m=video") != NULL);
    *has_audio = (strstr(sdp, "m=audio") != NULL);
}

// 从 SDP 中提取 interleaved 通道
void extract_interleaved_channels(const char *sdp, int *video_channel, int *audio_channel) {
    *video_channel = -1;
    *audio_channel = -1;
    
    const char *video_line = strstr(sdp, "m=video");
    if (video_line) {
        const char *control_line = strstr(video_line, "a=control:streamid=");
        if (control_line) {
            int stream_id = atoi(control_line + 19);
            *video_channel = stream_id * 2;
        }
    }
    
    const char *audio_line = strstr(sdp, "m=audio");
    if (audio_line) {
        const char *control_line = strstr(audio_line, "a=control:streamid=");
        if (control_line) {
            int stream_id = atoi(control_line + 19);
            *audio_channel = stream_id * 2;
        }
    }
}

/**
 * @brief 接收 TCP Interleaved RTP 包
 * @param socket TCP socket 描述符
 * @param channel 输出参数，RTP 通道号
 * @param buffer 接收缓冲区
 * @param max_len 缓冲区最大长度
 * @return 接收到的 payload 长度，失败返回 -1
 *
 * @数据格式
 *   TCP Interleaved 格式: $ + channel(1 byte) + length(2 bytes) + RTP payload
 *
 * @示例
 *   接收到的数据: 0x24 0x00 0x05 0xDC [RTP payload 1500 bytes]
 *   表示: channel=0, length=1500
 */
int receive_interleaved_rtp(int socket, int *channel, uint8_t *buffer, int max_len) {
    uint8_t header[4];
    int header_received = 0;
    
    while (header_received < 4) {
        int bytes = recv(socket, header + header_received, 4 - header_received, 0);
        if (bytes <= 0) {
            return -1;
        }
        header_received += bytes;
    }
    
    *channel = header[1];
    int payload_len = (header[2] << 8) | header[3];
    
    if (payload_len > max_len) {
        return -1;
    }
    
    int payload_received = 0;
    while (payload_received < payload_len) {
        int bytes = recv(socket, buffer + payload_received, payload_len - payload_received, 0);
        if (bytes <= 0) {
            return -1;
        }
        payload_received += bytes;
    }
    
    return payload_received;
}

/**
 * @brief 解析 H264 RTP 包
 * @param data RTP 数据包
 * @param len 数据包长度
 * @param timestamp RTP 时间戳
 * @param seq RTP 序列号
 *
 * @功能说明
 *   解析 RTP 头部，提取 H264 NALU 信息。
 *   支持的 NALU 类型:
 *   - 1: 非 IDR 图像片
 *   - 5: IDR 图像片 (关键帧)
 *   - 28: FU-A 分片单元
 *
 * @输出信息
 *   打印 SEQ、TS、NALU 类型和大小
 */
void parse_h264_rtp(const uint8_t *data, int len, uint32_t timestamp, uint16_t seq) {
    if (len < 12) return;
    
    uint8_t v = (data[0] >> 6) & 0x03;
    uint8_t pt = data[1] & 0x7F;
    uint16_t seq_num = (data[2] << 8) | data[3];
    uint32_t ts = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    
    int payload_offset = 12;
    if (v == 2) {
        uint8_t cc = data[0] & 0x0F;
        payload_offset += 4 * cc;
    }
    
    if (len <= payload_offset) return;
    
    uint8_t *payload = (uint8_t *)data + payload_offset;
    int payload_len = len - payload_offset;
    
    uint8_t nal_type = payload[0] & 0x1F;
    
    printf("[H264] SEQ=%d TS=%u NALU=%d 大小=%d\n", seq_num, ts, nal_type, payload_len);
}

/**
 * @brief 解析 AAC RTP 包
 * @param data RTP 数据包
 * @param len 数据包长度
 * @param timestamp RTP 时间戳
 * @param seq RTP 序列号
 *
 * @功能说明
 *   解析 RFC 3640 格式的 AAC RTP 包。
 *   提取 AU headers 和音频数据。
 *
 * @AU Header 格式
 *   - AU-headers-length: 16 bits，表示 AU headers 的总位数
 *   - AU-header: 16 bits = AU-size (13 bits) + AU-Index (3 bits)
 *
 * @输出信息
 *   打印 SEQ、TS、AU 索引和大小
 */
void parse_aac_rtp(const uint8_t *data, int len, uint32_t timestamp, uint16_t seq) {
    if (len < 12) return;
    
    uint8_t pt = data[1] & 0x7F;
    uint16_t seq_num = (data[2] << 8) | data[3];
    uint32_t ts = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    
    int payload_offset = 12;
    if ((data[0] >> 6) == 2) {
        uint8_t cc = data[0] & 0x0F;
        payload_offset += 4 * cc;
    }
    
    if (len <= payload_offset) return;
    
    uint8_t *payload = (uint8_t *)data + payload_offset;
    int payload_len = len - payload_offset;
    
    if (payload_len < 4) return;
    
    uint16_t au_headers_length = (payload[0] << 8) | payload[1];
    int au_headers_bits = au_headers_length;
    int au_header_size = 16;
    int num_au = au_headers_bits / au_header_size;
    
    int au_offset = 2;
    
    for (int i = 0; i < num_au && au_offset < payload_len; i++) {
        uint16_t au_header = (payload[au_offset] << 8) | payload[au_offset + 1];
        int au_size = (au_header >> 3) & 0x1FFF;
        int au_index = au_header & 0x07;
        
        au_offset += 2;
        
        if (au_offset + au_size <= payload_len) {
            printf("[AAC] SEQ=%d TS=%u AU=%d 大小=%d\n", seq_num, ts, i, au_size);
        }
        
        au_offset += au_size;
    }
}

// RTP 数据接收线程
void *receive_thread(void *arg) {
    RTSPClient *client = (RTSPClient *)arg;
    uint8_t buffer[MAX_RTP_PAYLOAD];
    int video_count = 0;
    int audio_count = 0;
    time_t start_time = time(NULL);
    int error_count = 0;
    
    printf("RTP 接收线程启动 (TCP interleaved)\n");
    printf("等待 PLAY 请求...\n");
    
    while (client->running) {
        int channel;
        int payload_len = receive_interleaved_rtp(client->socket, &channel, buffer, sizeof(buffer));
        
        if (payload_len < 0) {
            error_count++;
            if (error_count > 10) {
                if (client->running) {
                    printf("接收数据失败，连续错误 %d 次\n", error_count);
                }
                break;
            }
            usleep(10000);
            continue;
        }
        
        error_count = 0;
        
        if (channel == client->video_interleaved) {
            parse_h264_rtp(buffer, payload_len, 0, 0);
            video_count++;
        } else if (channel == client->audio_interleaved) {
            parse_aac_rtp(buffer, payload_len, 0, 0);
            audio_count++;
        }
        
        time_t current_time = time(NULL);
        if (current_time - start_time >= 1) {
            printf("统计: 视频=%d 音频=%d\n", video_count, audio_count);
            start_time = current_time;
            video_count = 0;
            audio_count = 0;
        }
    }
    
    printf("RTP 接收线程结束\n");
    return NULL;
}

// 打印使用说明
void print_usage(const char *prog_name) {
    printf("用法: %s <rtsp_url> [duration]\n", prog_name);
    printf("参数:\n");
    printf("  rtsp_url   - RTSP 流地址 (例如: rtsp://192.168.2.113:7554/inrico/test_stream)\n");
    printf("  duration   - 拉流时长，单位秒 (默认: 30)\n");
    printf("示例:\n");
    printf("  %s rtsp://192.168.2.113:7554/inrico/test_stream\n", prog_name);
    printf("  %s rtsp://192.168.2.113:7554/inrico/test_stream 60\n", prog_name);
}

// 信号处理函数
void signal_handler(int sig) {
    printf("\n接收到信号 %d，程序即将退出...\n", sig);
    exit(0);
}

// 主函数
int main(int argc, char *argv[]) {
    RTSPClient client;
    char response[4096];
    char request[1024];
    int duration = 30;
    
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    if (argc < 2) {
        print_usage(argv[0]);
        return -1;
    }
    
    const char *rtsp_url = argv[1];
    if (argc >= 3) {
        duration = atoi(argv[2]);
    }
    
    printf("RTSP URL: %s\n", rtsp_url);
    printf("拉流时长: %d 秒\n", duration);
    
    memset(&client, 0, sizeof(client));
    client.cseq = 1;
    client.state = RTSP_STATE_INIT;
    client.running = 1;
    strcpy(client.url, rtsp_url);
    
    if (parse_rtsp_url(client.url, client.server_ip, &client.server_port, client.stream_path) < 0) {
        fprintf(stderr, "无效的 RTSP URL\n");
        return -1;
    }
    
    client.socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client.socket < 0) {
        perror("创建 socket 失败");
        return -1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(client.server_port);
    if (inet_pton(AF_INET, client.server_ip, &server_addr.sin_addr) <= 0) {
        perror("无效的服务器 IP");
        close(client.socket);
        return -1;
    }
    
    if (connect(client.socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("连接服务器失败");
        close(client.socket);
        return -1;
    }
    
    printf("已连接到服务器 %s:%d\n", client.server_ip, client.server_port);
    
    while (client.state != RTSP_STATE_FINISH) {
        switch (client.state) {
            case RTSP_STATE_INIT:
                client.state = RTSP_STATE_OPTIONS;
                break;
                
            case RTSP_STATE_OPTIONS:
                /**
                 * @brief OPTIONS 步骤
                 * @作用
                 *   查询服务器支持的 RTSP 方法
                 *   确认服务器是否支持拉流所需的方法（DESCRIBE, SETUP, PLAY 等）
                 * @请求格式
                 *   OPTIONS rtsp://server/stream RTSP/1.0
                 *   CSeq: 1
                 *   User-Agent: C-RTSP-Client
                 * @预期响应
                 *   RTSP/1.0 200 OK
                 *   Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, etc.
                 * @重要性
                 *   必须先执行此步骤，确保服务器支持拉流操作
                 */
                snprintf(request, sizeof(request), 
                         "OPTIONS %s RTSP/1.0\r\n"
                         "CSeq: %d\r\n"
                         "User-Agent: C-RTSP-Client\r\n"
                         "\r\n",
                         client.url, client.cseq++);
                send_rtsp_request(client.socket, request);
                receive_rtsp_response(client.socket, response, sizeof(response));
                printf("OPTIONS 响应:\n%s\n", response);
                client.state = RTSP_STATE_DESCRIBE;
                break;
                
            case RTSP_STATE_DESCRIBE:
                /**
                 * @brief DESCRIBE 步骤
                 * @作用
                 *   获取服务器的 SDP (Session Description Protocol) 描述
                 *   了解流的媒体信息（编码、格式、传输方式等）
                 * @请求格式
                 *   DESCRIBE rtsp://server/stream RTSP/1.0
                 *   CSeq: 2
                 *   User-Agent: C-RTSP-Client
                 *   Accept: application/sdp
                 * @预期响应
                 *   RTSP/1.0 200 OK
                 *   Content-Type: application/sdp
                 *   Content-Length: <length>
                 *   
                 *   v=0
                 *   o=- 0 0 IN IP4 127.0.0.1
                 *   s=TestStream
                 *   c=IN IP4 0.0.0.0
                 *   t=0 0
                 *   m=video 0 RTP/AVP 96
                 *   a=rtpmap:96 H264/90000
                 *   a=fmtp:96 packetization-mode=1;...
                 *   a=control:streamid=0
                 *   m=audio 0 RTP/AVP 97
                 *   a=rtpmap:97 MPEG4-GENERIC/44100/2
                 *   a=fmtp:97 profile-level-id=1;...
                 *   a=control:streamid=1
                 * @重要性
                 *   必须获取 SDP 信息，以了解流的具体参数和可用的媒体轨道
                 */
                snprintf(request, sizeof(request), 
                         "DESCRIBE %s RTSP/1.0\r\n"
                         "CSeq: %d\r\n"
                         "User-Agent: C-RTSP-Client\r\n"
                         "Accept: application/sdp\r\n"
                         "\r\n",
                         client.url, client.cseq++);
                send_rtsp_request(client.socket, request);
                int len = receive_rtsp_response(client.socket, response, sizeof(response));
                printf("DESCRIBE 响应 (长度=%d):\n%s\n", len, response);
                
                if (len > 0 && strstr(response, "200 OK")) {
                    char *sdp_start = strstr(response, "\r\n\r\n");
                    if (sdp_start) {
                        sdp_start += 4;
                        strcpy(client.sdp, sdp_start);
                        printf("SDP 内容:\n%s\n", client.sdp);
                    } else {
                        strcpy(client.sdp, response);
                    }
                    extract_media_info(client.sdp, &client.has_video, &client.has_audio);
                    extract_interleaved_channels(client.sdp, &client.video_interleaved, &client.audio_interleaved);
                    printf("媒体信息: 视频=%d 音频=%d\n", client.has_video, client.has_audio);
                    printf("Interleaved 通道: 视频=%d 音频=%d\n", client.video_interleaved, client.audio_interleaved);
                    client.state = RTSP_STATE_SETUP;
                } else {
                    fprintf(stderr, "DESCRIBE 失败\n");
                    client.state = RTSP_STATE_TEARDOWN;
                }
                break;
                
            case RTSP_STATE_SETUP:
                /**
                 * @brief SETUP 步骤
                 * @作用
                 *   为每个媒体轨道建立 RTP 传输通道
                 *   协商传输协议（TCP/UDP）和端口
                 *   服务器会分配会话 ID
                 * @视频轨道设置
                 *   - URL: rtsp://server/stream/streamid=0
                 *   - Transport: RTP/AVP/TCP;unicast;interleaved=0-1
                 *   - interleaved=0-1: 视频 RTP 数据用通道 0，RTCP 用通道 1
                 * @音频轨道设置
                 *   - URL: rtsp://server/stream/streamid=1
                 *   - Transport: RTP/AVP/TCP;unicast;interleaved=2-3
                 *   - interleaved=2-3: 音频 RTP 数据用通道 2，RTCP 用通道 3
                 * @重要性
                 *   必须为每个媒体轨道单独设置，确保传输通道正确建立
                 */
                if (client.has_video) {
                    snprintf(request, sizeof(request), 
                             "SETUP %s/streamid=0 RTSP/1.0\r\n"
                             "CSeq: %d\r\n"
                             "User-Agent: C-RTSP-Client\r\n"
                             "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                             "\r\n",
                             client.url, client.cseq++);
                    send_rtsp_request(client.socket, request);
                    receive_rtsp_response(client.socket, response, sizeof(response));
                    printf("SETUP 视频响应:\n%s\n", response);
                    extract_session_id(response, client.session_id);
                }
                
                if (client.has_audio) {
                    snprintf(request, sizeof(request), 
                             "SETUP %s/streamid=1 RTSP/1.0\r\n"
                             "CSeq: %d\r\n"
                             "User-Agent: C-RTSP-Client\r\n"
                             "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n"
                             "Session: %s\r\n"
                             "\r\n",
                             client.url, client.cseq++, client.session_id);
                    send_rtsp_request(client.socket, request);
                    receive_rtsp_response(client.socket, response, sizeof(response));
                    printf("SETUP 音频响应:\n%s\n", response);
                }
                
                client.state = RTSP_STATE_PLAY;
                break;
                
            case RTSP_STATE_PLAY:
                /**
                 * @brief PLAY 步骤
                 * @作用
                 *   通知服务器开始发送 RTP 数据
                 *   服务器开始推送音视频流
                 * @请求格式
                 *   PLAY rtsp://server/stream RTSP/1.0
                 *   Session: <session_id>
                 *   Range: npt=0.000- (从开始到结束)
                 * @后续操作
                 *   启动 RTP 接收线程，开始接收和解析 RTP 数据包
                 * @重要性
                 *   这是开始实际接收流的信号，服务器会开始推送数据
                 */
                // 先启动接收线程，准备接收数据
                pthread_create(&client.receive_thread, NULL, receive_thread, &client);
                usleep(100000); // 给线程一点时间初始化
                
                snprintf(request, sizeof(request), 
                         "PLAY %s RTSP/1.0\r\n"
                         "CSeq: %d\r\n"
                         "User-Agent: C-RTSP-Client\r\n"
                         "Session: %s\r\n"
                         "Range: npt=0.000-\r\n"
                         "\r\n",
                         client.url, client.cseq++, client.session_id);
                send_rtsp_request(client.socket, request);
                receive_rtsp_response(client.socket, response, sizeof(response));
                printf("PLAY 响应:\n%s\n", response);
                
                printf("开始接收流...\n");
                sleep(duration); // 接收指定时长的流
                
                client.state = RTSP_STATE_TEARDOWN;
                break;
                
            case RTSP_STATE_TEARDOWN:
                /**
                 * @brief TEARDOWN 步骤
                 * @作用
                 *   通知服务器结束拉流会话
                 *   释放相关资源
                 * @请求格式
                 *   TEARDOWN rtsp://server/stream RTSP/1.0
                 *   Session: <session_id>
                 * @重要性
                 *   必须执行此步骤以正确关闭会话，否则服务器可能会保持资源占用
                 */
                client.running = 0; // 停止接收线程
                pthread_join(client.receive_thread, NULL); // 等待线程结束
                
                snprintf(request, sizeof(request), 
                         "TEARDOWN %s RTSP/1.0\r\n"
                         "CSeq: %d\r\n"
                         "User-Agent: C-RTSP-Client\r\n"
                         "Session: %s\r\n"
                         "\r\n",
                         client.url, client.cseq++, client.session_id);
                send_rtsp_request(client.socket, request);
                receive_rtsp_response(client.socket, response, sizeof(response));
                printf("TEARDOWN 响应:\n%s\n", response);
                client.state = RTSP_STATE_FINISH;
                break;
                
            default:
                client.state = RTSP_STATE_FINISH;
                break;
        }
    }
    
    close(client.socket);
    printf("连接已关闭\n");
    
    return 0;
}
