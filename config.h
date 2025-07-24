#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <chrono>

class Config {
private:
    std::unordered_map<std::string, std::string> config_map;
    
    void load_env_file(const std::string& filename) {
        std::ifstream file(filename);
        std::string line;
        
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            
            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                config_map[key] = value;
            }
        }
    }

public:
    Config(const std::string& config_file = "config.env") {
        load_env_file(config_file);
    }
    
    std::string get(const std::string& key, const std::string& default_value = "") const {
        auto it = config_map.find(key);
        return (it != config_map.end()) ? it->second : default_value;
    }
    
    // 설정값 접근 함수들
    std::string mqtt_server_address() const { return get("MQTT_SERVER_ADDRESS", "tcp://localhost:1883"); }
    std::string mqtt_topic() const { return get("MQTT_TOPIC", "factory/#"); }
    std::string query_request_topic() const { return get("QUERY_REQUEST_TOPIC", "factory/query/logs/request"); }
    std::string query_response_topic() const { return get("QUERY_RESPONSE_TOPIC", "factory/query/logs/response"); }
    std::string statistics_request_topic() const { return get("STATISTICS_REQUEST_TOPIC", "factory/statistics"); }

    std::string mongo_uri() const { return get("MONGO_URI", "mongodb://localhost:27017"); }
    std::string mongo_db_name() const { return get("MONGO_DB_NAME", "factory_monitoring"); }
    std::string devices_collection() const { return get("DEVICES_COLLECTION", "devices"); }
    std::string all_logs_collection() const { return get("ALL_LOGS_COLLECTION", "logs_all"); }
    std::string statistics_collection() const { return get("STATISTICS_COLLECTION", "statistics"); }
    
    std::string mqtt_client_id() const {
        return "factory_monitor_db_writer_" + 
               std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count());
    }
};