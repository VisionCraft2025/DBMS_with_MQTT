#!/bin/bash

# 통계 요청 테스트 스크립트
# 사용법: ./test_statistics.sh [device_id]

DEVICE_ID=${1:-"conveyor_01"}
REQUEST_ID=$(date +%s%N | cut -b1-13)

echo "Sending statistics request for device: $DEVICE_ID with request_id: $REQUEST_ID"

# MQTT 메시지 발행
mosquitto_pub -h mqtt.kwon.pics -p 1883 -t "factory/statistics" -m "{\"device_id\":\"$DEVICE_ID\",\"request_id\":\"$REQUEST_ID\"}"

echo "Request sent. Check the logs for response."