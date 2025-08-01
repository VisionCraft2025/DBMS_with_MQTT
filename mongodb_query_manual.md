# MongoDB 조회 명령어 메뉴얼

## 기본 연결
```bash
mongosh
use factory_monitoring
```

## 1. 개수 조회

### 전체 개수
```javascript
db.logs_all.countDocuments({})
db.videos.countDocuments({})
```

### 조건부 개수
```javascript
// 특정 날짜 범위
const start = new Date('2025-07-01').getTime();
const end = new Date('2025-08-01').getTime();
db.logs_all.countDocuments({timestamp: {$gte: start, $lt: end}})

// 특정 디바이스
db.logs_all.countDocuments({device_id: "feeder_01"})

// 특정 로그 코드
db.logs_all.countDocuments({log_code: "SPD"})
```

## 2. 데이터 조회

### 최신 데이터
```javascript
db.logs_all.find().sort({timestamp: -1}).limit(5)
db.videos.find().sort({video_created_time: -1}).limit(5)
```

### 조건 검색
```javascript
// 특정 디바이스의 최근 로그
db.logs_all.find({device_id: "feeder_01"}).sort({timestamp: -1}).limit(10)

// 에러 로그만
db.logs_all.find({log_level: "error"})

// 특정 시간 범위
db.logs_all.find({
  timestamp: {
    $gte: new Date('2025-07-25').getTime(),
    $lt: new Date('2025-07-26').getTime()
  }
})
```

## 3. 집계 쿼리

### 디바이스별 통계
```javascript
db.logs_all.aggregate([
  {$group: {_id: "$device_id", count: {$sum: 1}}},
  {$sort: {count: -1}}
])
```

### 날짜별 통계
```javascript
db.logs_all.aggregate([
  {$addFields: {
    date: {$dateToString: {
      format: "%Y-%m-%d",
      date: {$toDate: "$timestamp"}
    }}
  }},
  {$group: {_id: "$date", count: {$sum: 1}}},
  {$sort: {_id: 1}}
])
```

### 영상 용량 통계
```javascript
db.videos.aggregate([
  {$group: {
    _id: "$device_id",
    count: {$sum: 1},
    totalSize: {$sum: "$file_size"}
  }}
])
```

## 4. 시간 범위 조회 패턴

### 오늘
```javascript
const today = new Date();
today.setHours(0,0,0,0);
const tomorrow = new Date(today);
tomorrow.setDate(tomorrow.getDate() + 1);

db.logs_all.countDocuments({
  timestamp: {$gte: today.getTime(), $lt: tomorrow.getTime()}
})
```

### 특정 월
```javascript
const monthStart = new Date('2025-07-01').getTime();
const monthEnd = new Date('2025-08-01').getTime();

db.logs_all.countDocuments({
  timestamp: {$gte: monthStart, $lt: monthEnd}
})
```

## 5. 삭제 쿼리

### 조건부 삭제
```javascript
// 특정 날짜 이전 데이터 삭제
db.logs_all.deleteMany({
  timestamp: {$lt: new Date('2025-07-01').getTime()}
})

// 특정 디바이스 데이터 삭제
db.videos.deleteMany({device_id: "old_device"})
```

## 6. 컬렉션 관리

### 컬렉션 목록
```javascript
show collections
```

### 인덱스 확인
```javascript
db.logs_all.getIndexes()
```

### 컬렉션 크기
```javascript
db.logs_all.stats()
```