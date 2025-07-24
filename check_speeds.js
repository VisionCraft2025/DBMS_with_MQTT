// MongoDB 쿼리 스크립트: 각 기기별 속도 및 평균 속도 확인

print("===== 기기별 속도 통계 =====");

// 모든 SPD 로그가 있는 디바이스 목록 조회
const devices = db.logs_all.distinct("device_id", {
  log_code: "SPD",
  message: { $regex: /^[0-9]+$/ }
});

print(`총 ${devices.length}개 디바이스 발견\n`);

// 각 디바이스별 통계 계산
devices.forEach(deviceId => {
  // 디바이스 정보 조회
  const deviceInfo = db.devices.findOne({ _id: deviceId });
  const deviceName = deviceInfo ? deviceInfo.device_name : "알 수 없음";
  
  // 평균 속도 계산
  const avgResult = db.logs_all.aggregate([
    { 
      $match: { 
        device_id: deviceId,
        log_code: "SPD",
        message: { $regex: /^[0-9]+$/ }
      } 
    },
    { 
      $addFields: { 
        speed_value: { $toDouble: "$message" } 
      } 
    },
    { 
      $group: { 
        _id: null, 
        average: { $avg: "$speed_value" },
        count: { $sum: 1 }
      } 
    }
  ]).toArray();
  
  const average = avgResult.length > 0 ? Math.round(avgResult[0].average) : 0;
  const count = avgResult.length > 0 ? avgResult[0].count : 0;
  
  // 현재(최신) 속도 조회
  const latestDoc = db.logs_all.find({
    device_id: deviceId,
    log_code: "SPD",
    message: { $regex: /^[0-9]+$/ }
  }).sort({ timestamp: -1 }).limit(1).toArray();
  
  const currentSpeed = latestDoc.length > 0 ? parseInt(latestDoc[0].message) : 0;
  
  print(`디바이스: ${deviceId} (${deviceName})`);
  print(`  - 평균 속도: ${average} (총 ${count}개 로그)`);
  print(`  - 현재 속도: ${currentSpeed}`);
  print("");
});

print("==========================");