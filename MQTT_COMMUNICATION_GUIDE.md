# MQTT 통신 가이드라인

이 문서는 Factory Monitor MQTT 시스템과 통신하는 방법을 설명합니다.

## 1. 로그 데이터 전송 (Device → System)

### 1.1 토픽 형식
```
factory/{device_id}/log/{log_level}
```

**예시:**
- `factory/robot_arm_01/log/info`
- `factory/conveyor_02/log/warning`
- `factory/feeder_03/log/error`

### 1.2 페이로드 형식 (JSON)
```json
{
  "log_code": "TMP",                    // 필수: 로그 코드 (TMP, SPD, COL 등)
  "message": "Temperature exceeded",     // 필수: 로그 메시지
  "timestamp": 1722153600000,           // 선택: Unix timestamp (ms), 없으면 수신시간 사용
  "metadata": {                         // 선택: 추가 데이터
    "temperature": 85.5,
    "threshold": 80.0,
    "sensor_id": "temp_01"
  }
}
```

### 1.3 지원되는 로그 코드
- `TMP`: 온도 관련
- `SPD`: 속도 관련  
- `COL`: 충돌 관련
- `UNKNOWN`: 기타

### 1.4 전송 예시 (Python)
```python
import json
import paho.mqtt.client as mqtt

# MQTT 클라이언트 설정
client = mqtt.Client()
client.connect("mqtt.kwon.pics", 1883, 60)

# 로그 데이터 전송
payload = {
    "log_code": "TMP",
    "message": "Temperature warning: 85.5°C",
    "metadata": {
        "temperature": 85.5,
        "threshold": 80.0,
        "sensor_id": "temp_sensor_01"
    }
}

topic = "factory/robot_arm_01/log/warning"
client.publish(topic, json.dumps(payload))
```

### 1.5 전송 예시 (JavaScript/Node.js)
```javascript
const mqtt = require('mqtt');
const client = mqtt.connect('mqtt://mqtt.kwon.pics:1883');

const payload = {
    log_code: "SPD",
    message: "Speed decreased below threshold",
    metadata: {
        current_speed: 45.2,
        expected_speed: 50.0,
        motor_id: "motor_02"
    }
};

const topic = "factory/conveyor_02/log/info";
client.publish(topic, JSON.stringify(payload));
```

## 2. 로그 조회 요청 (Client → System)

### 2.1 요청 토픽
```
factory/query/logs/request
```

### 2.2 요청 페이로드 형식
```json
{
  "query_id": "unique_query_identifier",  // 필수: 고유 쿼리 ID
  "query_type": "logs",                   // 필수: "logs" 고정값
  "filters": {                            // 선택: 필터 조건들
    "device_id": "robot_arm_01",          // 특정 디바이스
    "log_level": "error",                 // 로그 레벨 (info, warning, error)
    "log_code": "TMP",                    // 로그 코드
    "severity": "HIGH",                   // 심각도 (LOW, MEDIUM, HIGH, CRITICAL)
    "time_range": {                       // 시간 범위
      "start": 1722096000000,             // Unix timestamp (ms)
      "end": 1722182400000
    },
    "limit": 50                           // 결과 개수 제한 (기본값: 100)
  }
}
```

### 2.3 조회 요청 예시 (Python)
```python
import json
import uuid
import paho.mqtt.client as mqtt

def on_message(client, userdata, msg):
    if msg.topic == "factory/query/logs/response":
        response = json.loads(msg.payload.decode())
        print(f"Query {response['query_id']} result: {response['count']} logs found")
        for log in response.get('data', []):
            print(f"- {log['timestamp']}: {log['message']}")

client = mqtt.Client()
client.on_message = on_message
client.connect("mqtt.kwon.pics", 1883, 60)
client.subscribe("factory/query/logs/response")

# 최근 에러 로그 조회
query = {
    "query_id": str(uuid.uuid4()),
    "query_type": "logs",
    "filters": {
        "log_level": "error",
        "limit": 10
    }
}

client.publish("factory/query/logs/request", json.dumps(query))
client.loop_forever()
```

## 3. 로그 조회 응답 (System → Client)

### 3.1 응답 토픽
```
factory/query/logs/response
```

### 3.2 성공 응답 형식
```json
{
  "query_id": "unique_query_identifier",
  "status": "success",
  "count": 25,
  "data": [
    {
      "_id": "RA01-TMP-01HVCXYZ123456789",
      "device_id": "robot_arm_01",
      "device_name": "Robot Arm #1",
      "log_level": "warning",
      "log_code": "TMP",
      "severity": "HIGH",
      "message": "Temperature exceeded threshold",
      "location": "Factory Line A",
      "timestamp": 1722153600000
    }
    // ... 더 많은 로그 데이터
  ]
}
```

### 3.3 오류 응답 형식
```json
{
  "query_id": "unique_query_identifier",
  "status": "error",
  "error": "Unsupported query type"
}
```

## 4. 실제 사용 시나리오

### 4.1 시나리오 1: 온도 경고 로그 전송
```python
# 디바이스에서 온도 경고 발생
payload = {
    "log_code": "TMP",
    "message": "Temperature warning: 85.5°C detected",
    "metadata": {
        "temperature": 85.5,
        "threshold": 80.0,
        "sensor_location": "motor_housing"
    }
}
client.publish("factory/robot_arm_01/log/warning", json.dumps(payload))
```

### 4.2 시나리오 2: 특정 디바이스의 최근 에러 조회
```python
query = {
    "query_id": "check_robot_errors_20250728",
    "query_type": "logs", 
    "filters": {
        "device_id": "robot_arm_01",
        "log_level": "error",
        "time_range": {
            "start": 1722096000000,  # 24시간 전
            "end": 1722182400000     # 현재
        },
        "limit": 20
    }
}
client.publish("factory/query/logs/request", json.dumps(query))
```

### 4.3 시나리오 3: 모든 CRITICAL 로그 조회
```python
query = {
    "query_id": "critical_logs_check",
    "query_type": "logs",
    "filters": {
        "severity": "CRITICAL",
        "limit": 100
    }
}
client.publish("factory/query/logs/request", json.dumps(query))
```

## 5. 중요 참고사항

### 5.1 디바이스 등록
- 로그를 전송하기 전에 해당 `device_id`가 MongoDB의 `devices` 컬렉션에 등록되어 있어야 함
- 등록되지 않은 디바이스의 로그는 무시됨

### 5.2 Severity 자동 계산
- 시스템이 `log_code`와 `metadata`, 디바이스 설정을 기반으로 자동 계산
- TMP 로그의 경우 온도값과 임계값을 비교하여 LOW/MEDIUM/HIGH/CRITICAL 결정

### 5.3 데이터 저장 위치
- 그룹별 컬렉션: `logs_factory_line_a_robots`, `logs_factory_line_a_conveyors` 등
- 통합 컬렉션: `logs_all` (모든 로그 통합 저장)

### 5.4 MQTT 연결 정보
- **브로커**: `mqtt.kwon.pics:1883`
- **QoS**: 1 (최소 한 번 전달 보장)
- **Clean Session**: true

### 5.5 응답 처리
- 조회 요청 시 반드시 응답 토픽을 구독해야 함
- `query_id`를 통해 요청과 응답을 매칭
- 비동기 처리이므로 적절한 타임아웃 설정 필요

## 6. 테스트 도구

### mosquitto_pub을 사용한 테스트
```bash
# 로그 전송 테스트
mosquitto_pub -h mqtt.kwon.pics -t "factory/test_device/log/info" \
  -m '{"log_code":"TMP","message":"Test message","metadata":{"temperature":25.0}}'

# 조회 요청 테스트  
mosquitto_pub -h mqtt.kwon.pics -t "factory/query/logs/request" \
  -m '{"query_id":"test123","query_type":"logs","filters":{"limit":5}}'

# 응답 확인
mosquitto_sub -h mqtt.kwon.pics -t "factory/query/logs/response"
```

이 가이드라인을 따라 MQTT 클라이언트를 구현하면 Factory Monitor 시스템과 원활하게 통신할 수 있습니다.
