# MQTT to MongoDB Bridge

MQTT 메시지를 받아서 MongoDB에 저장하는 C++ 애플리케이션입니다.

## 빌드 요구사항

```bash
# Ubuntu/Debian
sudo apt install libmongocxx-dev libbsoncxx-dev libpaho-mqtt-dev libpaho-mqttpp-dev nlohmann-json3-dev

# 또는 직접 설치
# MongoDB C++ Driver: https://github.com/mongodb/mongo-cxx-driver
# Paho MQTT C++: https://github.com/eclipse/paho.mqtt.cpp
```

## 빌드 및 실행

```bash
mkdir build && cd build
cmake ..
make
./mqtt_to_mongodb
```

## 기능

- **MQTT 구독**: `factory/+/log/+` 토픽 모니터링
- **메시지 파싱**: 디바이스 타입, 로그 레벨, 에러 코드 자동 분석
- **구조화된 ID**: `RA01-TMP-ULID` 형식으로 생성
- **MongoDB 저장**: 디바이스별 컬렉션 + 통합 컬렉션
- **Severity 계산**: 에러 타입별 심각도 자동 판단

## 설정

- **MQTT 브로커**: `mqtt.kwon.pics:1883`
- **MongoDB**: `localhost:27017/factory_monitoring`

## 컬렉션 구조

- `logs_factory_line_a_robots`: 로봇팔 로그
- `logs_factory_line_a_conveyors`: 컨베이어 로그  
- `logs_factory_line_a_feeders`: 피더 로그
- `logs_all`: 전체 통합 로그