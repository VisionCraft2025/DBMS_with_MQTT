#!/bin/bash

# 특정 디바이스에 대한 통계 요청 테스트 스크립트 (시간 범위 포함)
# 사용법: ./test_statistics_with_range.sh [device_id]

DEVICE_ID=${1:-"conveyor_01"}
REQUEST_ID=$(date +%s%N | cut -b1-13)

# 7월 23일부터 현재까지의 시간 범위 설정
START_TIME=1690070400000  # 2023-07-23 00:00:00 UTC
END_TIME=$(date +%s%3N)   # 현재 시간 (밀리초)

echo "Sending statistics request for device: $DEVICE_ID with request_id: $REQUEST_ID"
echo "Time range: $(date -d @$((START_TIME/1000))) to $(date)"

# MQTT 메시지 발행
mosquitto_pub -h mqtt.kwon.pics -p 1883 -t "factory/statistics" -m "{\"device_id\":\"$DEVICE_ID\",\"request_id\":\"$REQUEST_ID\",\"time_range\":{\"start\":$START_TIME,\"end\":$END_TIME}}"

echo "Request sent. Check the logs for response."