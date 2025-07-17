#pragma once
#include <string>
#include <optional>
#include <nlohmann/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/database.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <mqtt/async_client.h>
#include "config.h"

using json = nlohmann::json;

class DatabaseManager {
private:
    const Config& config;
    
public:
    DatabaseManager(const Config& cfg);
    
    // 디바이스 정보 조회
    bsoncxx::stdx::optional<bsoncxx::document::value> get_device_info(
        mongocxx::database& db, const std::string& device_id);
    
    // Severity 계산
    std::string determine_severity(const std::string& log_code, 
                                 const json& metadata, 
                                 const bsoncxx::document::view& device_info);
    
    // 쿼리 처리
    void process_query_request(mongocxx::client& mongo_client, 
                             mqtt::async_client* mqtt_client, 
                             const json& query);
    
    // 로그 저장
    void save_log_to_mongodb(mongocxx::database& db, 
                           const std::string& device_id,
                           const std::string& log_level,
                           const json& payload,
                           const std::string& topic,
                           const bsoncxx::document::view& device_info);
};