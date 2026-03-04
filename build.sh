#!/bin/bash

# 编译 RTSP 推流端
echo "编译 RTSP 推流端..."
gcc -o rtsp_pusher rtsp_pusher.c -lavformat -lavcodec -lavutil -lswscale -lpthread

if [ $? -eq 0 ]; then
    echo "RTSP 推流端编译成功"
else
    echo "RTSP 推流端编译失败"
    exit 1
fi

# 编译 RTSP 拉流端
echo "编译 RTSP 拉流端..."
gcc -o rtsp_client rtsp_client.c -lpthread

if [ $? -eq 0 ]; then
    echo "RTSP 拉流端编译成功"
else
    echo "RTSP 拉流端编译失败"
    exit 1
fi

echo "编译完成！"
echo ""
echo "生成的可执行文件:"
echo "  - rtsp_pusher (推流端)"
echo "  - rtsp_client (拉流端)"
