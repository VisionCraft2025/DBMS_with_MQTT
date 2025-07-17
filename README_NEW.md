# MQTT to MongoDB Bridge (리팩토링 버전)

MQTT 메시지를 받아서 MongoDB에 저장하는 C++ 애플리케이션입니다.

## 파일 구조

```
db_mqtt/
├── config.env              # 환경 설정 파일
├── config.h               # 설정 관리 클래스
├── database_manager.h/cpp # MongoDB 관련 기능
├── mqtt_handler.h/cpp     # MQTT 메시지 처리
├── main.cpp              # 메인 프로그램
├── CMakeLists.txt        # 빌드 설정
└── README_NEW.md         # 이 파일
```

## 설정 파일 (config.env)

환경변수 형태로 설정값을 관리합니다:

```env
# MQTT Configuration
MQTT_SERVER_ADDRESS=tcp://mqtt.kwon.pics:1883
MQTT_TOPIC=factory/#
QUERY_REQUEST_TOPIC=factory/query/logs/request
QUERY_RESPONSE_TOPIC=factory/query/logs/response

# MongoDB Configuration
MONGO_URI=mongodb://localhost:27017
MONGO_DB_NAME=factory_monitoring
DEVICES_COLLECTION=devices
ALL_LOGS_COLLECTION=logs_all
```

## 빌드 및 실행

```bash
mkdir build && cd build
cmake ..
make
./db_mqtt
```

## 주요 개선사항

1. **모듈화**: 기능별로 파일 분리
2. **설정 외부화**: 하드코딩된 설정값을 config.env로 분리
3. **관심사 분리**: MQTT, DB, 메인 로직 독립적 관리
4. **재사용성**: 각 모듈을 다른 프로젝트에서 활용 가능

## 클래스 구조

- **Config**: 설정 파일 로드 및 관리
- **DatabaseManager**: MongoDB 연결, 쿼리, 로그 저장
- **MqttHandler**: MQTT 메시지 수신 및 처리