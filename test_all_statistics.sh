#!/bin/bash

# 모든 디바이스에 대한 통계 요청 테스트 스크립트
REQUEST_ID=$(date +%s%N | cut -b1-13)

echo "Sending statistics request for ALL devices with request_id: $REQUEST_ID"

# 7월 23일부터 현재까지의 시간 범위 설정
START_TIME=1690070400000  # 2023-07-23 00:00:00 UTC
END_TIME=$(date +%s%3N)   # 현재 시간 (밀리초)

# MQTT 메시지 발행
mosquitto_pub -h mqtt.kwon.pics -p 1883 -t "factory/statistics" -m "{\"device_id\":\"All\",\"request_id\":\"$REQUEST_ID\",\"time_range\":{\"start\":$START_TIME,\"end\":$END_TIME}}"

echo "Request sent. Check the logs for response."