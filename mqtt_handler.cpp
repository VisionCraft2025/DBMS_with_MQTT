#include "mqtt_handler.h"
#include <iostream>
#include <regex>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

MqttHandler::MqttHandler(mongocxx::client& client, 
                        mqtt::async_client* mqtt_client, 
                        const Config& cfg,
                        DatabaseManager& db_mgr) 
    : mongo_client(client), mqtt_client(mqtt_client), config(cfg), db_manager(db_mgr) {
    load_device_states();
}

void MqttHandler::connected(const std::string& cause) {
    std::cout << "MQTT Connected!" << std::endl;
    // 연결 성공 시 토픽 구독
    if (mqtt_client) {
        mqtt_client->subscribe(config.mqtt_topic(), 1);
        mqtt_client->subscribe(config.query_request_topic(), 1);
        mqtt_client->subscribe(config.statistics_request_topic(), 1);

        std::cout << "Subscribed to topics: " << config.mqtt_topic() 
                  << ", " << config.query_request_topic() 
                  << ", " << config.statistics_request_topic() << std::endl;    }
}

void MqttHandler::connection_lost(const std::string& cause) {
    std::cerr << "MQTT Connection lost: " << cause << std::endl;
}

void MqttHandler::message_arrived(mqtt::const_message_ptr msg) {
    try {
        std::string topic_str = msg->get_topic();
        
        // 쿼리 요청 처리
        if (topic_str == config.query_request_topic()) {
            json query = json::parse(msg->get_payload_str());
            std::cout << "Processing query request: " << query.value("query_id", "unknown") << std::endl;
            db_manager.process_query_request(mongo_client, mqtt_client, query);
            return;
        }

        // 통계 요청 처리
        if (topic_str == config.statistics_request_topic()) {
            json request = json::parse(msg->get_payload_str());
            std::cout << "Processing statistics request for: " << request.value("device_id", "unknown") << std::endl;
            db_manager.process_statistics_request(mongo_client, mqtt_client, request);
            return;
        }

        // 토픽 파싱 (factory/{device_id}/...)
        std::regex topic_regex("factory/([^/]+)/");
        std::smatch matches;
        if (!std::regex_search(topic_str, matches, topic_regex) || matches.size() != 2) {
            return;
        }
        std::string device_id = matches[1].str();

        // 페이로드 파싱
        json payload = json::parse(msg->get_payload_str());
        std::string log_code = payload.value("log_code", "");

        // SHD/STR 처리 (shutdown 상태 확인보다 먼저)
        if (log_code == "SHD") {
            std::string msg_device_id = payload.value("message", "");
            if (msg_device_id == device_id) {
                set_device_shutdown(device_id);
            }
            return;
        }

        if (log_code == "STR") {
            set_device_active(device_id);
            // STR 메시지는 계속 처리하여 DB에 저장
        }

        // shutdown 상태 확인 (STR 처리 후)
        if (is_device_shutdown(device_id)) {
            return; // 조용히 무시
        }

        // 로그 토픽 파싱 (factory/{device_id}/log/{log_level})
        std::regex log_topic_regex("factory/([^/]+)/log/([^/]+)");
        std::smatch log_matches;
        if (!std::regex_match(topic_str, log_matches, log_topic_regex) || log_matches.size() != 3) {
            return;
        }
        std::string log_level = log_matches[2].str();

        std::cout << "Message arrived on topic: " << topic_str << std::endl;

        // MongoDB에서 디바이스 정보 조회
        auto db = mongo_client[config.mongo_db_name()];
        auto device_info_opt = db_manager.get_device_info(db, device_id);
        if (!device_info_opt) {
            std::cerr << "Device '" << device_id << "' not found in DB. Skipping." << std::endl;
            return;
        }
        auto device_info = device_info_opt->view();

        // 로그 저장
        db_manager.save_log_to_mongodb(db, device_id, log_level, payload, topic_str, device_info);

    } catch (const json::parse_error& e) {
        std::cerr << "JSON parse error: " << e.what() << " on topic: " << msg->get_topic() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "An error occurred in message_arrived: " << e.what() << std::endl;
    }
}

void MqttHandler::load_device_states() {
    std::ifstream file(state_file);
    std::string device_id;
    while (std::getline(file, device_id)) {
        if (!device_id.empty()) {
            shutdown_devices.insert(device_id);
        }
    }
    std::cout << "Loaded " << shutdown_devices.size() << " shutdown devices" << std::endl;
}

void MqttHandler::save_device_states() {
    std::ofstream file(state_file);
    for (const auto& device_id : shutdown_devices) {
        file << device_id << "\n";
    }
}

void MqttHandler::set_device_shutdown(const std::string& device_id) {
    if (shutdown_devices.insert(device_id).second) {
        save_device_states();
        std::cout << "Device " << device_id << " marked as shutdown" << std::endl;
    }
}

void MqttHandler::set_device_active(const std::string& device_id) {
    if (shutdown_devices.erase(device_id)) {
        save_device_states();
        std::cout << "Device " << device_id << " started" << std::endl;
    }
}

bool MqttHandler::is_device_shutdown(const std::string& device_id) const {
    return shutdown_devices.count(device_id) > 0;
}