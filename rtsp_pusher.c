/**
 * @file rtsp_pusher.c
 * @brief RTSP 推流客户端 - 将 MP4 文件推送到 RTSP 服务器
 *
 * @功能说明
 *   本程序实现了一个完整的 RTSP 推流客户端，支持：
 *   - H264 视频编码推流
 *   - AAC 音频编码推流
 *   - TCP 传输模式（RTP over RTSP）
 *   - 文件循环播放
 *   - PTS 时间戳同步
 *
 * @支持的媒体格式
 *   - 视频: H264 (AVCC 格式自动转换为 Annex B)
 *   - 音频: AAC-LC (带 AU headers 的 RTP 封装)
 *   - 容器: MP4
 *
 * @编译方法
 *   gcc -o rtsp_pusher rtsp_pusher.c -lavformat -lavcodec -lavutil -lswscale -lpthread
 *   或使用: ./build_rtsp.sh
 *
 * @使用方法
 *   ./rtsp_pusher <mp4文件路径> <rtsp服务器地址>
 *
 * @示例
 *   ./rtsp_pusher /home/user/video/test.mp4 rtsp://192.168.2.113:7554/inrico/test_stream
 *
 * @RTSP 信令流程
 *   1. OPTIONS    - 查询服务器支持的方法
 *   2. ANNOUNCE   - 发送 SDP 描述媒体信息
 *   3. SETUP      - 建立 RTP 传输通道
 *   4. RECORD     - 开始推流
 *   5. TEARDOWN   - 结束推流
 *
 * @RTP 封装格式
 *   - 视频: H264 RTP 封装 (RFC 6184)
 *     * 支持 FU-A 分片传输大包
 *     * 支持 NALU 直接传输小包
 *   - 音频: AAC RTP 封装 (RFC 3640)
 *     * AU-headers-length: 16 bits
 *     * AU-header: AU-size (13 bits) + AU-Index (3 bits)
 *
 * @作者
 *   基于 FFmpeg 和 RTSP 协议实现
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
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

// RTSP 客户端状态
typedef enum {
    RTSP_STATE_INIT,
    RTSP_STATE_OPTIONS,
    RTSP_STATE_ANNOUNCE,
    RTSP_STATE_SETUP,
    RTSP_STATE_RECORD,
    RTSP_STATE_TEARDOWN,
    RTSP_STATE_FINISH
} RTSPState;

// RTSP 客户端结构
typedef struct {
    int socket;
    char session_id[128];
    int cseq;
    RTSPState state;
    char url[512];
    char server_ip[64];
    int server_port;
    char stream_path[256];
    int video_rtp_port;
    int video_rtcp_port;
    int audio_rtp_port;
    int audio_rtcp_port;
    int video_rtp_socket;
    int audio_rtp_socket;
    struct sockaddr_in video_server_addr;
    struct sockaddr_in audio_server_addr;
    pthread_t video_thread;
    pthread_t audio_thread;
    int running;
    AVFormatContext *input_ctx;
    int video_stream_index;
    int audio_stream_index;
    uint32_t video_timestamp;
    uint32_t audio_timestamp;
    uint8_t video_sps_pps[256];
    int video_sps_pps_len;
} RTSPClient;

// 解析 RTSP URL
int parse_rtsp_url(const char *url, char *server_ip, int *server_port, char *stream_path) {
    char tmp_url[512];
    strcpy(tmp_url, url);
    
    char *host_start = strstr(tmp_url, "rtsp://");
    if (!host_start) {
        fprintf(stderr, "无效的 RTSP URL，缺少 rtsp:// 前缀\n");
        return -1;
    }
    host_start += 7;
    
    char *port_start = strchr(host_start, ':');
    char *path_start = strchr(host_start, '/');
    
    if (!path_start) {
        fprintf(stderr, "无效的 RTSP URL，缺少流路径\n");
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
    printf("解析 RTSP URL: IP=%s, 端口=%d, 路径=%s\n", server_ip, *server_port, stream_path);
    return 0;
}

// 发送 RTSP 请求
int send_rtsp_request(int socket, const char *request) {
    printf("发送 RTSP 请求:\n%s\n", request);
    int bytes_sent = send(socket, request, strlen(request), 0);
    if (bytes_sent < 0) {
        perror("发送 RTSP 请求失败");
    }
    return bytes_sent;
}

// 接收 RTSP 响应
int receive_rtsp_response(int socket, char *response, int max_len) {
    int bytes_received = recv(socket, response, max_len - 1, 0);
    if (bytes_received > 0) {
        response[bytes_received] = '\0';
        printf("接收 RTSP 响应:\n%s\n", response);
    } else if (bytes_received == 0) {
        printf("服务器关闭了连接\n");
    } else {
        perror("接收 RTSP 响应失败");
    }
    return bytes_received;
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
        } else {
            // 如果没有找到 \r，则复制到字符串末尾
            strcpy(session_id, session_line);
        }
    }
}

// 从响应中提取服务器端口
void extract_server_port(const char *response, int *rtp_port, int *rtcp_port) {
    const char *transport_line = strstr(response, "Transport:");
    if (transport_line) {
        const char *server_port = strstr(transport_line, "server_port=");
        if (server_port) {
            sscanf(server_port + 12, "%d-%d", rtp_port, rtcp_port);
        }
    }
}

// 创建 UDP socket
int create_udp_socket(int port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("创建 UDP socket 失败");
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("绑定 UDP 端口失败");
        close(sock);
        return -1;
    }
    
    return sock;
}

// 发送 RTP 数据包 (UDP)
void send_rtp_packet(int socket, struct sockaddr_in *server_addr, uint8_t *data, int len, 
                     uint8_t pt, uint32_t timestamp, uint32_t ssrc, int marker) {
    uint8_t rtp_header[12];
    
    rtp_header[0] = 0x80 | (marker ? 0x80 : 0x00);
    rtp_header[1] = pt;
    rtp_header[2] = (timestamp >> 24) & 0xFF;
    rtp_header[3] = (timestamp >> 16) & 0xFF;
    rtp_header[4] = (timestamp >> 8) & 0xFF;
    rtp_header[5] = timestamp & 0xFF;
    rtp_header[6] = (ssrc >> 24) & 0xFF;
    rtp_header[7] = (ssrc >> 16) & 0xFF;
    rtp_header[8] = (ssrc >> 8) & 0xFF;
    rtp_header[9] = ssrc & 0xFF;
    rtp_header[10] = 0;
    rtp_header[11] = 0;
    
    uint8_t packet[1500];
    memcpy(packet, rtp_header, 12);
    memcpy(packet + 12, data, len);
    
    sendto(socket, packet, len + 12, 0, (struct sockaddr *)server_addr, sizeof(struct sockaddr_in));
}

// 发送 RTP over TCP 数据包 (Interleaved 格式)
void send_rtp_over_tcp(int socket, uint8_t channel, uint8_t *data, int len, 
                       uint8_t pt, uint32_t timestamp, uint32_t ssrc, int marker, uint16_t seq_num) {
    uint8_t rtp_header[12];
    
    // RTP header 格式:
    // Byte 0: Version (2 bits) + Padding (1 bit) + Extension (1 bit) + CSRC count (4 bits)
    // Byte 1: Marker (1 bit) + Payload type (7 bits)
    // Bytes 2-3: Sequence number
    // Bytes 4-7: Timestamp
    // Bytes 8-11: SSRC
    
    rtp_header[0] = 0x80;  // Version 2, no padding, no extension, 0 CSRC
    rtp_header[1] = pt | (marker ? 0x80 : 0x00);  // Payload type + marker bit
    rtp_header[2] = (seq_num >> 8) & 0xFF;  // Sequence number high byte
    rtp_header[3] = seq_num & 0xFF;         // Sequence number low byte
    rtp_header[4] = (timestamp >> 24) & 0xFF;  // Timestamp high byte
    rtp_header[5] = (timestamp >> 16) & 0xFF;
    rtp_header[6] = (timestamp >> 8) & 0xFF;
    rtp_header[7] = timestamp & 0xFF;          // Timestamp low byte
    rtp_header[8] = (ssrc >> 24) & 0xFF;       // SSRC high byte
    rtp_header[9] = (ssrc >> 16) & 0xFF;
    rtp_header[10] = (ssrc >> 8) & 0xFF;
    rtp_header[11] = ssrc & 0xFF;              // SSRC low byte
    
    // RTP over TCP 格式: $ + channel (1 byte) + length (2 bytes) + RTP packet
    uint8_t tcp_header[4];
    tcp_header[0] = '$';
    tcp_header[1] = channel;
    uint16_t rtp_len = 12 + len;
    tcp_header[2] = (rtp_len >> 8) & 0xFF;
    tcp_header[3] = rtp_len & 0xFF;
    
    // 发送 TCP header
    send(socket, tcp_header, 4, 0);
    // 发送 RTP header
    send(socket, rtp_header, 12, 0);
    // 发送数据
    send(socket, data, len, 0);
}

// 将 AVCC 格式 (MP4) 转换为 Annex B 格式
// AVCC 格式: [4-byte length][NALU data]...
// Annex B 格式: [0x00 0x00 0x00 0x01][NALU data]...
int avcc_to_annexb(uint8_t *src, int src_len, uint8_t *dst, int dst_size) {
    int src_pos = 0;
    int dst_pos = 0;
    
    while (src_pos < src_len) {
        // 读取 NALU 长度 (4 bytes, big-endian)
        if (src_pos + 4 > src_len) break;
        uint32_t nalu_len = (src[src_pos] << 24) | (src[src_pos + 1] << 16) |
                           (src[src_pos + 2] << 8) | src[src_pos + 3];
        src_pos += 4;
        
        if (src_pos + nalu_len > src_len) break;
        if (dst_pos + 4 + nalu_len > dst_size) break;
        
        // 写入 start code
        dst[dst_pos++] = 0x00;
        dst[dst_pos++] = 0x00;
        dst[dst_pos++] = 0x00;
        dst[dst_pos++] = 0x01;
        
        // 复制 NALU 数据
        memcpy(dst + dst_pos, src + src_pos, nalu_len);
        dst_pos += nalu_len;
        src_pos += nalu_len;
    }
    
    return dst_pos;
}

// 发送单个 NALU，支持 FU-A 分片
void send_nalu(int socket, uint8_t channel, uint8_t *data, int len,
               uint8_t pt, uint32_t timestamp, uint32_t ssrc, uint16_t *seq_num) {
    const int MAX_RTP_PAYLOAD = 1400;
    
    // 如果数据较小，直接发送
    if (len <= MAX_RTP_PAYLOAD) {
        send_rtp_over_tcp(socket, channel, data, len, pt, timestamp, ssrc, 1, (*seq_num)++);
        return;
    }
    
    // 需要分片发送 (FU-A)
    uint8_t nal_header = data[0];
    uint8_t nal_type = nal_header & 0x1F;
    uint8_t nal_nri = nal_header & 0x60;
    
    // FU indicator: nal_nri | 28 (FU-A)
    uint8_t fu_indicator = nal_nri | 28;
    
    int pos = 1;  // 跳过原始 NAL header
    int remaining = len - 1;
    int is_first = 1;
    
    while (remaining > 0) {
        int payload_size = (remaining > MAX_RTP_PAYLOAD - 2) ? (MAX_RTP_PAYLOAD - 2) : remaining;
        int is_last = (remaining <= MAX_RTP_PAYLOAD - 2);
        
        // FU header: S (1) | E (1) | R (1) | Type (5)
        uint8_t fu_header = nal_type;
        if (is_first) fu_header |= 0x80;  // Set S bit
        if (is_last) fu_header |= 0x40;   // Set E bit
        
        // 构建 FU-A payload
        uint8_t fu_payload[MAX_RTP_PAYLOAD];
        fu_payload[0] = fu_indicator;
        fu_payload[1] = fu_header;
        memcpy(fu_payload + 2, data + pos, payload_size);
        
        // 发送 FU-A 包
        send_rtp_over_tcp(socket, channel, fu_payload, payload_size + 2, pt, timestamp, ssrc, 
                         is_last ? 1 : 0, (*seq_num)++);
        
        pos += payload_size;
        remaining -= payload_size;
        is_first = 0;
    }
}

/**
 * @brief 发送 H264 视频帧
 * @param socket TCP socket 描述符
 * @param channel RTP over TCP 通道号 (0 表示视频)
 * @param data 视频帧数据 (AVCC 格式)
 * @param len 数据长度
 * @param pt RTP payload type (96 表示 H264)
 * @param timestamp RTP 时间戳
 * @param ssrc RTP SSRC 标识符
 * @param seq_num RTP 序列号指针
 *
 * @说明
 *   将 AVCC 格式的 H264 数据转换为 Annex B 格式，然后逐个 NALU 发送。
 *   大 NALU 会自动分片为 FU-A 格式传输。
 */
void send_h264_frame(int socket, uint8_t channel, uint8_t *data, int len,
                     uint8_t pt, uint32_t timestamp, uint32_t ssrc, uint16_t *seq_num) {
    // 临时缓冲区用于存储 Annex B 格式数据
    static uint8_t annexb_buffer[1024 * 1024];  // 1MB 缓冲区
    
    // 将 AVCC 转换为 Annex B
    int annexb_len = avcc_to_annexb(data, len, annexb_buffer, sizeof(annexb_buffer));
    if (annexb_len <= 0) return;
    
    // 解析 Annex B 格式的数据，逐个 NALU 发送
    int pos = 0;
    while (pos < annexb_len) {
        // 查找 start code
        if (annexb_buffer[pos] != 0x00 || annexb_buffer[pos+1] != 0x00 || 
            annexb_buffer[pos+2] != 0x00 || annexb_buffer[pos+3] != 0x01) {
            pos++;
            continue;
        }
        
        // 找到 start code，跳过它
        pos += 4;
        int nalu_start = pos;
        
        // 查找下一个 start code 或结束
        while (pos < annexb_len) {
            if (pos + 4 <= annexb_len && 
                annexb_buffer[pos] == 0x00 && annexb_buffer[pos+1] == 0x00 &&
                annexb_buffer[pos+2] == 0x00 && annexb_buffer[pos+3] == 0x01) {
                break;
            }
            pos++;
        }
        
        int nalu_len = pos - nalu_start;
        if (nalu_len > 0) {
            send_nalu(socket, channel, annexb_buffer + nalu_start, nalu_len, 
                     pt, timestamp, ssrc, seq_num);
        }
    }
}

// 推流线程 (单线程读取文件，分别发送视频和音频)
/**
 * @brief 推流线程主函数
 * @param arg RTSPClient 结构体指针
 * @return NULL
 *
 * @功能说明
 *   从 MP4 文件读取音视频帧，按照 PTS 时间戳同步发送。
 *   支持文件循环播放，到达文件末尾自动回到开头。
 *
 * @同步机制
 *   - 记录起始时间 (start_pts 和 start_time_us)
 *   - 计算当前帧应该发送的时间
 *   - 根据延迟进行睡眠等待
 */
void *push_thread(void *arg) {
    RTSPClient *client = (RTSPClient *)arg;
    AVPacket packet;
    uint16_t video_seq = 0;
    uint16_t audio_seq = 0;
    
    printf("推流线程启动 (TCP)\n");
    
    // 获取视频和音频流的信息
    AVStream *video_stream = NULL;
    AVStream *audio_stream = NULL;
    int video_fps = 30;
    int audio_sample_rate = 44100;
    
    if (client->video_stream_index >= 0) {
        video_stream = client->input_ctx->streams[client->video_stream_index];
        if (video_stream->avg_frame_rate.den > 0) {
            video_fps = video_stream->avg_frame_rate.num / video_stream->avg_frame_rate.den;
        }
        printf("视频流: %dx%d, %d fps\n", 
               video_stream->codecpar->width,
               video_stream->codecpar->height,
               video_fps);
    }
    
    if (client->audio_stream_index >= 0) {
        audio_stream = client->input_ctx->streams[client->audio_stream_index];
        if (audio_stream->codecpar->sample_rate > 0) {
            audio_sample_rate = audio_stream->codecpar->sample_rate;
        }
        printf("音频流: %d Hz\n", audio_sample_rate);
    }
    
    // 记录起始时间
    int64_t start_pts = -1;
    int64_t start_time_us = av_gettime();
    int64_t last_pts = -1;
    
    while (client->running) {
        int ret = av_read_frame(client->input_ctx, &packet);
        if (ret < 0) {
            // 文件读取完毕，重新定位到文件开头
            if (ret == AVERROR_EOF) {
                printf("文件读取完毕，重新开始\n");
                av_seek_frame(client->input_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
                client->video_timestamp = 0;
                client->audio_timestamp = 0;
                start_pts = -1;
                start_time_us = av_gettime();
                last_pts = -1;
                continue;
            } else {
                printf("读取错误: %d\n", ret);
                av_usleep(100000);
                continue;
            }
        }
        
        AVStream *stream = client->input_ctx->streams[packet.stream_index];
        int64_t pts = packet.pts;
        
        // 初始化起始时间
        if (start_pts == -1) {
            start_pts = pts;
            start_time_us = av_gettime();
        }
        
        // 计算当前帧相对于起始时间的延迟
        int64_t pts_diff = pts - start_pts;
        int64_t time_base = stream->time_base.den / stream->time_base.num;
        int64_t expected_time_us = (pts_diff * 1000000) / time_base;
        int64_t current_time_us = av_gettime() - start_time_us;
        int64_t delay_us = expected_time_us - current_time_us;
        
        // 如果延迟大于 0，则等待
        if (delay_us > 0) {
            av_usleep(delay_us);
        }
        
        if (packet.stream_index == client->video_stream_index) {
            // 视频帧 - 使用 PTS 计算 RTP 时间戳 (90kHz 时钟)
            // RTP 时间戳 = PTS * (90000 / time_base)
            if (video_stream) {
                client->video_timestamp = (uint32_t)(pts * 90000 / video_stream->time_base.den * video_stream->time_base.num);
            } else {
                int timestamp_increment = 90000 / video_fps;
                client->video_timestamp += timestamp_increment;
            }
            
            // 使用 H264 分片发送
            send_h264_frame(client->socket, 0, packet.data, packet.size, 
                           96, client->video_timestamp, 0x12345678, &video_seq);
        } 
        else if (packet.stream_index == client->audio_stream_index) {
            // 音频帧 - AAC RTP 封装 (RFC 3640)
            // 使用 PTS 计算 RTP 时间戳 (采样率时钟)
            if (audio_stream) {
                client->audio_timestamp = (uint32_t)(pts * audio_sample_rate / audio_stream->time_base.den * audio_stream->time_base.num);
            } else {
                int timestamp_increment = 1024;  // AAC 每帧 1024 采样点
                client->audio_timestamp += timestamp_increment;
            }
            
            // 构建 AU headers section
            // 1. AU-headers-length: 16 bits, 表示 AU headers 的总位数
            //    1 个 AU header = 16 bits
            // 2. AU-header: 16 bits = AU-size (13 bits) + AU-Index (3 bits)
            
            int au_size = packet.size;
            uint8_t au_headers[4];
            
            // AU-headers-length = 16 (1 个 AU header, 16 bits)
            au_headers[0] = 0x00;  // 高 8 bits
            au_headers[1] = 0x10;  // 低 8 bits (16 bits = 0x0010)
            
            // AU-header: AU-size (13 bits) + AU-Index (3 bits)
            // AU-Index = 0 表示第一个 AU
            au_headers[2] = (au_size >> 5) & 0xFF;  // AU-size 高 8 bits
            au_headers[3] = ((au_size << 3) & 0xF8);  // AU-size 低 5 bits + AU-Index (000)
            
            // 构建完整的 RTP payload
            uint8_t *rtp_payload = malloc(4 + packet.size);
            if (rtp_payload) {
                memcpy(rtp_payload, au_headers, 4);
                memcpy(rtp_payload + 4, packet.data, packet.size);
                
                send_rtp_over_tcp(client->socket, 2, rtp_payload, 4 + packet.size,
                                  97, client->audio_timestamp, 0x87654321, 1, audio_seq++);
                
                free(rtp_payload);
            }
        }
        
        last_pts = pts;
        av_packet_unref(&packet);
    }
    
    printf("推流线程结束\n");
    return NULL;
}

// 打印使用说明
void print_usage(const char *prog_name) {
    printf("用法: %s <mp4_file> <rtsp_url>\n", prog_name);
    printf("示例: %s test.mp4 rtsp://192.168.2.113/inrico/test_stream\n", prog_name);
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
    
    // 忽略 SIGPIPE 信号，防止连接断开时程序崩溃
    signal(SIGPIPE, SIG_IGN);
    // 处理 SIGINT 信号 (Ctrl+C)
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 初始化客户端结构体
    memset(&client, 0, sizeof(client));
    
    if (argc != 3) {
        print_usage(argv[0]);
        return -1;
    }
    
    const char *mp4_file = argv[1];
    const char *rtsp_url = argv[2];
    
    printf("MP4 文件: %s\n", mp4_file);
    printf("RTSP URL: %s\n", rtsp_url);
    
    // 初始化 FFmpeg
    avformat_network_init();
    
    // 打开 MP4 文件
    if (avformat_open_input(&client.input_ctx, mp4_file, NULL, NULL) != 0) {
        fprintf(stderr, "无法打开 MP4 文件: %s\n", mp4_file);
        return -1;
    }
    
    if (avformat_find_stream_info(client.input_ctx, NULL) < 0) {
        fprintf(stderr, "无法获取流信息\n");
        avformat_close_input(&client.input_ctx);
        return -1;
    }
    
    // 查找视频和音频流
    client.video_stream_index = -1;
    client.audio_stream_index = -1;
    
    for (unsigned int i = 0; i < client.input_ctx->nb_streams; i++) {
        if (client.input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && 
            client.video_stream_index == -1) {
            client.video_stream_index = i;
        } else if (client.input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && 
                   client.audio_stream_index == -1) {
            client.audio_stream_index = i;
        }
    }
    
    if (client.video_stream_index == -1) {
        fprintf(stderr, "未找到视频流\n");
        avformat_close_input(&client.input_ctx);
        return -1;
    }
    
    // 提取 SPS/PPS 用于 SDP
    client.video_sps_pps_len = 0;
    if (client.video_stream_index >= 0) {
        AVCodecParameters *codecpar = client.input_ctx->streams[client.video_stream_index]->codecpar;
        if (codecpar->extradata && codecpar->extradata_size > 0) {
            // 将 AVCC 格式的 extradata 转换为 base64 编码的 SPS/PPS
            // AVCC 格式: [6-byte header][2-byte SPS len][SPS][1-byte PPS count][2-byte PPS len][PPS]
            uint8_t *extra = codecpar->extradata;
            int extra_size = codecpar->extradata_size;
            
            if (extra_size >= 7) {
                int numSPS = extra[5] & 0x1F;
                int pos = 6;
                char sps_b64[256] = {0};
                char pps_b64[256] = {0};
                int sps_b64_len = 0;
                int pps_b64_len = 0;
                
                // 提取 SPS
                for (int i = 0; i < numSPS && pos < extra_size; i++) {
                    if (pos + 2 > extra_size) break;
                    int spsLen = (extra[pos] << 8) | extra[pos + 1];
                    pos += 2;
                    if (pos + spsLen > extra_size) break;
                    
                    // Base64 编码 SPS
                    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    int j = 0;
                    while (j < spsLen) {
                        uint32_t a = (j < spsLen) ? extra[pos + j] : 0; j++;
                        uint32_t b = (j < spsLen) ? extra[pos + j] : 0; j++;
                        uint32_t c = (j < spsLen) ? extra[pos + j] : 0; j++;
                        uint32_t triple = (a << 16) | (b << 8) | c;
                        
                        if (sps_b64_len < sizeof(sps_b64) - 5) {
                            sps_b64[sps_b64_len++] = b64[(triple >> 18) & 0x3F];
                            sps_b64[sps_b64_len++] = b64[(triple >> 12) & 0x3F];
                            sps_b64[sps_b64_len++] = (j > spsLen + 1) ? '=' : b64[(triple >> 6) & 0x3F];
                            sps_b64[sps_b64_len++] = (j > spsLen) ? '=' : b64[triple & 0x3F];
                        }
                    }
                    sps_b64[sps_b64_len] = '\0';
                    pos += spsLen;
                }
                
                // 提取 PPS
                if (pos < extra_size) {
                    int numPPS = extra[pos++];
                    for (int i = 0; i < numPPS && pos < extra_size; i++) {
                        if (pos + 2 > extra_size) break;
                        int ppsLen = (extra[pos] << 8) | extra[pos + 1];
                        pos += 2;
                        if (pos + ppsLen > extra_size) break;
                        
                        // Base64 编码 PPS
                        static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        int j = 0;
                        while (j < ppsLen) {
                            uint32_t a = (j < ppsLen) ? extra[pos + j] : 0; j++;
                            uint32_t b = (j < ppsLen) ? extra[pos + j] : 0; j++;
                            uint32_t c = (j < ppsLen) ? extra[pos + j] : 0; j++;
                            uint32_t triple = (a << 16) | (b << 8) | c;
                            
                            if (pps_b64_len < sizeof(pps_b64) - 5) {
                                pps_b64[pps_b64_len++] = b64[(triple >> 18) & 0x3F];
                                pps_b64[pps_b64_len++] = b64[(triple >> 12) & 0x3F];
                                pps_b64[pps_b64_len++] = (j > ppsLen + 1) ? '=' : b64[(triple >> 6) & 0x3F];
                                pps_b64[pps_b64_len++] = (j > ppsLen) ? '=' : b64[triple & 0x3F];
                            }
                        }
                        pps_b64[pps_b64_len] = '\0';
                        pos += ppsLen;
                    }
                }
                
                // 组合 SPS 和 PPS
                snprintf((char*)client.video_sps_pps, sizeof(client.video_sps_pps), 
                         "%s,%s", sps_b64, pps_b64);
                client.video_sps_pps_len = strlen((char*)client.video_sps_pps);
                printf("SPS/PPS: %s\n", client.video_sps_pps);
            }
        }
    }
    
    // 初始化客户端的其他字段
    client.cseq = 1;
    client.state = RTSP_STATE_INIT;
    client.running = 1;
    client.video_timestamp = 0;
    client.audio_timestamp = 0;
    strcpy(client.url, rtsp_url);
    
    // 解析 RTSP URL
    if (parse_rtsp_url(client.url, client.server_ip, &client.server_port, client.stream_path) < 0) {
        fprintf(stderr, "无效的 RTSP URL\n");
        avformat_close_input(&client.input_ctx);
        return -1;
    }
    
    // 创建 socket
    client.socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client.socket < 0) {
        perror("创建 socket 失败");
        avformat_close_input(&client.input_ctx);
        return -1;
    }
    
    // 连接服务器
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(client.server_port);
    if (inet_pton(AF_INET, client.server_ip, &server_addr.sin_addr) <= 0) {
        perror("无效的服务器 IP");
        close(client.socket);
        avformat_close_input(&client.input_ctx);
        return -1;
    }
    
    if (connect(client.socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("连接服务器失败");
        close(client.socket);
        avformat_close_input(&client.input_ctx);
        return -1;
    }
    
    printf("已连接到服务器 %s:%d\n", client.server_ip, client.server_port);
    
    // 生成随机端口
    srand(time(NULL));
    client.video_rtp_port = 10000 + (rand() % 10000) * 2;
    client.video_rtcp_port = client.video_rtp_port + 1;
    client.audio_rtp_port = client.video_rtcp_port + 2;
    client.audio_rtcp_port = client.audio_rtp_port + 1;
    
    // RTSP 信令交互流程
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
                 *   确认服务器是否支持推流所需的方法（ANNOUNCE, RECORD 等）
                 * @请求格式
                 *   OPTIONS rtsp://server/stream RTSP/1.0
                 *   CSeq: 1
                 *   User-Agent: C-RTSP-Pusher
                 * @预期响应
                 *   RTSP/1.0 200 OK
                 *   Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, ANNOUNCE, RECORD, SET_PARAMETER, GET_PARAMETER
                 * @重要性
                 *   必须先执行此步骤，确保服务器支持推流操作
                 */
                snprintf(request, sizeof(request), 
                         "OPTIONS %s RTSP/1.0\r\n"
                         "CSeq: %d\r\n"
                         "User-Agent: C-RTSP-Pusher\r\n"
                         "\r\n",
                         client.url, client.cseq++);
                int bytes_sent = send_rtsp_request(client.socket, request);
                if (bytes_sent < 0) {
                    fprintf(stderr, "发送 OPTIONS 请求失败\n");
                    close(client.socket);
                    avformat_close_input(&client.input_ctx);
                    return -1;
                }
                int bytes_received = receive_rtsp_response(client.socket, response, sizeof(response));
                if (bytes_received <= 0) {
                    fprintf(stderr, "接收 OPTIONS 响应失败，服务器可能已关闭连接\n");
                    close(client.socket);
                    avformat_close_input(&client.input_ctx);
                    return -1;
                }
                client.state = RTSP_STATE_ANNOUNCE;
                break;
                
            case RTSP_STATE_ANNOUNCE:
                /**
                 * @brief ANNOUNCE 步骤
                 * @作用
                 *   向服务器发送 SDP (Session Description Protocol) 描述
                 *   告知服务器要推流的媒体信息（视频/音频编码、格式、参数等）
                 * @SDP 内容包括
                 *   - 视频：H264 编码，90kHz 时钟，包含 SPS/PPS 信息
                 *   - 音频：AAC 编码，44100Hz 采样率，包含 AudioSpecificConfig
                 * @请求格式
                 *   ANNOUNCE rtsp://server/stream RTSP/1.0
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
                 *   a=fmtp:96 packetization-mode=1;profile-level-id=640020;sprop-parameter-sets=...
                 *   a=control:streamid=0
                 *   m=audio 0 RTP/AVP 97
                 *   a=rtpmap:97 MPEG4-GENERIC/44100/2
                 *   a=fmtp:97 profile-level-id=1;mode=AAC-hbr;...
                 *   a=control:streamid=1
                 * @重要性
                 *   必须包含正确的 SDP 信息，否则服务器无法正确解码音视频
                 */
                {
                    char sdp[2048];
                    char video_sdp[512];
                    char audio_sdp[512];
                    
                    // 构建视频 SDP，包含 SPS/PPS
                    if (client.video_sps_pps_len > 0) {
                        snprintf(video_sdp, sizeof(video_sdp),
                                 "m=video 0 RTP/AVP 96\r\n"
                                 "a=rtpmap:96 H264/90000\r\n"
                                 "a=fmtp:96 packetization-mode=1;profile-level-id=640020;sprop-parameter-sets=%s\r\n"
                                 "a=control:streamid=0\r\n",
                                 client.video_sps_pps);
                    } else {
                        snprintf(video_sdp, sizeof(video_sdp),
                                 "m=video 0 RTP/AVP 96\r\n"
                                 "a=rtpmap:96 H264/90000\r\n"
                                 "a=control:streamid=0\r\n");
                    }
                    
                    // 构建音频 SDP
                    // 获取音频配置
                    int audio_rate = 44100;
                    int audio_channels = 2;
                    uint8_t asc[2] = {0x12, 0x10};  // 默认 AAC-LC 44100Hz 立体声
                    
                    if (client.audio_stream_index >= 0) {
                        AVCodecParameters *audio_codecpar = client.input_ctx->streams[client.audio_stream_index]->codecpar;
                        audio_rate = audio_codecpar->sample_rate;
                        audio_channels = audio_codecpar->channels;
                        if (audio_codecpar->extradata && audio_codecpar->extradata_size >= 2) {
                            memcpy(asc, audio_codecpar->extradata, 2);
                        }
                    }
                    
                    // 将 AudioSpecificConfig 转换为 hex 字符串 (用于 SDP 中的 config 参数)
                    char config_hex[16];
                    snprintf(config_hex, sizeof(config_hex), "%02x%02x", asc[0], asc[1]);
                    
                    snprintf(audio_sdp, sizeof(audio_sdp),
                             "m=audio 0 RTP/AVP 97\r\n"
                             "a=rtpmap:97 MPEG4-GENERIC/%d/%d\r\n"
                             "a=fmtp:97 profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3; config=%s\r\n"
                             "a=control:streamid=1\r\n",
                             audio_rate, audio_channels, config_hex);
                    
                    // 组合完整的 SDP 描述
                    snprintf(sdp, sizeof(sdp),
                             "v=0\r\n"
                             "o=- 0 0 IN IP4 127.0.0.1\r\n"
                             "s=TestStream\r\n"
                             "c=IN IP4 0.0.0.0\r\n"
                             "t=0 0\r\n"
                             "%s"
                             "%s",
                             video_sdp, audio_sdp);
                    int sdp_len = strlen(sdp);
                    snprintf(request, sizeof(request), 
                             "ANNOUNCE %s RTSP/1.0\r\n"
                             "CSeq: %d\r\n"
                             "User-Agent: C-RTSP-Pusher\r\n"
                             "Content-Type: application/sdp\r\n"
                             "Content-Length: %d\r\n"
                             "\r\n"
                             "%s",
                             client.url, client.cseq++, sdp_len, sdp);
                    send_rtsp_request(client.socket, request);
                    receive_rtsp_response(client.socket, response, sizeof(response));
                    extract_session_id(response, client.session_id); // 提取服务器分配的会话 ID
                    client.state = RTSP_STATE_SETUP;
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
                // SETUP 视频轨道 - 使用 TCP 传输
                {
                    char video_setup_url[1024];
                    snprintf(video_setup_url, sizeof(video_setup_url), "%s/streamid=0", client.url);
                    snprintf(request, sizeof(request), 
                             "SETUP %s RTSP/1.0\r\n"
                             "CSeq: %d\r\n"
                             "User-Agent: C-RTSP-Pusher\r\n"
                             "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                             "\r\n",
                             video_setup_url, client.cseq++);
                    send_rtsp_request(client.socket, request);
                    receive_rtsp_response(client.socket, response, sizeof(response));
                }
                
                // SETUP 音频轨道（如果存在）
                if (client.audio_stream_index != -1) {
                    char audio_setup_url[1024];
                    snprintf(audio_setup_url, sizeof(audio_setup_url), "%s/streamid=1", client.url);
                    snprintf(request, sizeof(request),
                             "SETUP %s RTSP/1.0\r\n"
                             "CSeq: %d\r\n"
                             "User-Agent: C-RTSP-Pusher\r\n"
                             "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n"
                             "Session: %s\r\n"
                             "\r\n",
                             audio_setup_url, client.cseq++, client.session_id);
                    send_rtsp_request(client.socket, request);
                    receive_rtsp_response(client.socket, response, sizeof(response));
                }

                client.state = RTSP_STATE_RECORD;
                break;
                
            case RTSP_STATE_RECORD:
                /**
                 * @brief RECORD 步骤
                 * @作用
                 *   通知服务器开始接收 RTP 数据
                 *   服务器准备接收推流数据
                 * @请求格式
                 *   RECORD rtsp://server/stream RTSP/1.0
                 *   Session: <session_id>
                 *   Range: npt=0.000- (从开始到结束)
                 * @后续操作
                 *   启动推流线程，开始发送 RTP 数据包
                 * @重要性
                 *   这是开始实际推流的信号，服务器会准备接收数据
                 */
                snprintf(request, sizeof(request), 
                         "RECORD %s RTSP/1.0\r\n"
                         "CSeq: %d\r\n"
                         "User-Agent: C-RTSP-Pusher\r\n"
                         "Session: %s\r\n"
                         "Range: npt=0.000-\r\n"
                         "\r\n",
                         client.url, client.cseq++, client.session_id);
                send_rtsp_request(client.socket, request);
                receive_rtsp_response(client.socket, response, sizeof(response));
                
                // 使用 TCP 传输，不需要创建 UDP socket
                // 启动推流线程 (单线程，同时处理视频和音频)
                pthread_create(&client.video_thread, NULL, push_thread, &client);
                
                printf("开始推流...\n");
                // 无限推流，直到用户中断
                while (1) {
                    sleep(1);
                }
                
                client.state = RTSP_STATE_TEARDOWN;
                break;
                
            case RTSP_STATE_TEARDOWN:
                /**
                 * @brief TEARDOWN 步骤
                 * @作用
                 *   通知服务器结束推流会话
                 *   释放相关资源
                 * @请求格式
                 *   TEARDOWN rtsp://server/stream RTSP/1.0
                 *   Session: <session_id>
                 * @重要性
                 *   必须执行此步骤以正确关闭会话，否则服务器可能会保持资源占用
                 */
                client.running = 0; // 停止推流线程
                
                pthread_join(client.video_thread, NULL); // 等待线程结束
                
                snprintf(request, sizeof(request), 
                         "TEARDOWN %s RTSP/1.0\r\n"
                         "CSeq: %d\r\n"
                         "User-Agent: C-RTSP-Pusher\r\n"
                         "Session: %s\r\n"
                         "\r\n",
                         client.url, client.cseq++, client.session_id);
                send_rtsp_request(client.socket, request);
                receive_rtsp_response(client.socket, response, sizeof(response));
                client.state = RTSP_STATE_FINISH;
                break;
                
            default:
                client.state = RTSP_STATE_FINISH;
                break;
        }
    }
    
    // 清理资源
    close(client.socket);
    avformat_close_input(&client.input_ctx);
    printf("连接已关闭\n");
    
    return 0;
}
