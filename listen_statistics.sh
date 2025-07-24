#!/bin/bash

# 통계 응답을 수신하는 스크립트
# 사용법: ./listen_statistics.sh [device_id]

DEVICE_ID=${1:-"conveyor_01"}
TOPIC="factory/${DEVICE_ID}/msg/statistics"

echo "Listening for statistics responses on topic: $TOPIC"
echo "Press Ctrl+C to exit"

# MQTT 메시지 구독
mosquitto_sub -h mqtt.kwon.pics -p 1883 -t "$TOPIC" -v