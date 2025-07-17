#include "database_manager.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <random>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/uri.hpp>

using bson_builder = bsoncxx::builder::stream::document;

// ULID 생성 함수 (utils에서 이동)
std::string generate_ulid() {
    static const char* ENCODING = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    uint64_t rand_high = dis(gen);
    uint64_t rand_low = dis(gen);

    char ulid[27];
    ulid[26] = 0;

    // Timestamp (48 bits)
    ulid[0] = ENCODING[(ms >> 45) & 0x1F];
    ulid[1] = ENCODING[(ms >> 40) & 0x1F];
    ulid[2] = ENCODING[(ms >> 35) & 0x1F];
    ulid[3] = ENCODING[(ms >> 30) & 0x1F];
    ulid[4] = ENCODING[(ms >> 25) & 0x1F];
    ulid[5] = ENCODING[(ms >> 20) & 0x1F];
    ulid[6] = ENCODING[(ms >> 15) & 0x1F];
    ulid[7] = ENCODING[(ms >> 10) & 0x1F];
    ulid[8] = ENCODING[(ms >> 5) & 0x1F];
    ulid[9] = ENCODING[ms & 0x1F];

    // Randomness (80 bits)
    auto encode_random = [&](uint64_t r, int start_idx) {
        for (int i = 0; i < 8; ++i) {
            ulid[start_idx + i] = ENCODING[r & 0x1F];
            r >>= 5;
        }
    };
    encode_random(rand_low, 10);
    encode_random(rand_high, 18);

    return std::string(ulid);
}

DatabaseManager::DatabaseManager(const Config& cfg) : config(cfg) {}

bsoncxx::stdx::optional<bsoncxx::document::value> DatabaseManager::get_device_info(
    mongocxx::database& db, const std::string& device_id) {
    try {
        auto collection = db[config.devices_collection()];
        bson_builder builder;
        builder << "_id" << device_id;
        return collection.find_one(builder.view());
    } catch (const std::exception& e) {
        std::cerr << "Error finding device '" << device_id << "': " << e.what() << std::endl;
        return bsoncxx::stdx::nullopt;
    }
}

std::string DatabaseManager::determine_severity(const std::string& log_code, 
                                              const json& metadata, 
                                              const bsoncxx::document::view& device_info) {
    try {
        if (!device_info["thresholds"]) return "UNKNOWN";
        auto thresholds = device_info["thresholds"].get_document().view();

        if (log_code == "TMP" && metadata.contains("temperature") && thresholds["temperature"]) {
            double temp = metadata["temperature"];
            if (temp >= thresholds["temperature"]["critical"].get_double()) return "CRITICAL";
            if (temp >= thresholds["temperature"]["high"].get_double()) return "HIGH";
            if (temp >= thresholds["temperature"]["medium"].get_double()) return "MEDIUM";
            return "LOW";
        }
        // 다른 log_code (COL, SPD 등)에 대한 규칙을 여기에 추가

    } catch (const std::exception& e) {
        std::cerr << "Error determining severity: " << e.what() << std::endl;
    }
    return "MEDIUM"; // 기본값
}

void DatabaseManager::process_query_request(mongocxx::client& mongo_client, 
                                          mqtt::async_client* mqtt_client, 
                                          const json& query) {
    try {
        std::string query_id = query.value("query_id", "");
        std::string query_type = query.value("query_type", "");
        
        if (query_type != "logs") {
            json error_response;
            error_response["query_id"] = query_id;
            error_response["status"] = "error";
            error_response["error"] = "Unsupported query type";
            std::string payload = error_response.dump();
            mqtt_client->publish(config.query_response_topic(), payload.c_str(), payload.length(), 1, false);
            return;
        }
        
        // 새로운 MongoDB 클라이언트 생성으로 최신 데이터 보장
        mongocxx::client fresh_client{mongocxx::uri{config.mongo_uri()}};
        auto db = fresh_client[config.mongo_db_name()];
        auto collection = db[config.all_logs_collection()];
        
        std::cout << "Processing query with fresh MongoDB connection..." << std::endl;
        
        // 필터 빌드
        using bsoncxx::builder::stream::document;
        using bsoncxx::builder::stream::finalize;
        
        auto filter_builder = document{};
        
        if (query.contains("filters")) {
            auto filters = query["filters"];
            
            if (filters.contains("device_id") && !filters["device_id"].empty()) {
                filter_builder << "device_id" << filters["device_id"].get<std::string>();
            }
            
            if (filters.contains("log_level") && !filters["log_level"].empty()) {
                filter_builder << "log_level" << filters["log_level"].get<std::string>();
            }
            
            if (filters.contains("log_code") && !filters["log_code"].empty()) {
                filter_builder << "log_code" << filters["log_code"].get<std::string>();
            }
            
            if (filters.contains("severity") && !filters["severity"].empty()) {
                filter_builder << "severity" << filters["severity"].get<std::string>();
            }
            
            if (filters.contains("time_range")) {
                auto time_range = filters["time_range"];
                if (time_range.contains("start") && time_range.contains("end")) {
                    int64_t start_time = time_range["start"];
                    int64_t end_time = time_range["end"];
                    
                    filter_builder << "timestamp" << bsoncxx::builder::stream::open_document
                                  << "$gte" << bsoncxx::types::b_int64{start_time}
                                  << "$lte" << bsoncxx::types::b_int64{end_time}
                                  << bsoncxx::builder::stream::close_document;
                }
            }
        }
        
        auto filter = filter_builder << finalize;
        
        // 제한 설정
        int limit = 100; // 기본값
        if (query.contains("filters") && query["filters"].contains("limit")) {
            limit = query["filters"]["limit"];
        }
        
        // 쿼리 실행
        mongocxx::options::find opts{};
        opts.limit(limit);
        opts.sort(document{} << "timestamp" << -1 << finalize); // 최신순 정렬
        
        auto cursor = collection.find(filter.view(), opts);
        
        // 결과 수집
        json response;
        response["query_id"] = query_id;
        response["status"] = "success";
        
        json data_array = json::array();
        int count = 0;
        
        for (auto&& doc : cursor) {
            json log_item;
            auto view = doc;
            
            if (view["_id"]) log_item["_id"] = std::string(view["_id"].get_string().value);
            if (view["device_id"]) log_item["device_id"] = std::string(view["device_id"].get_string().value);
            if (view["device_name"]) log_item["device_name"] = std::string(view["device_name"].get_string().value);
            if (view["log_level"]) log_item["log_level"] = std::string(view["log_level"].get_string().value);
            if (view["log_code"]) log_item["log_code"] = std::string(view["log_code"].get_string().value);
            if (view["severity"]) log_item["severity"] = std::string(view["severity"].get_string().value);
            if (view["message"]) log_item["message"] = std::string(view["message"].get_string().value);
            if (view["location"]) log_item["location"] = std::string(view["location"].get_string().value);
            if (view["timestamp"]) log_item["timestamp"] = view["timestamp"].get_int64().value;
            
            data_array.push_back(log_item);
            count++;
        }
        
        response["count"] = count;
        response["data"] = data_array;
        
        // 응답 전송
        std::string payload = response.dump();
        mqtt_client->publish(config.query_response_topic(), payload.c_str(), payload.length(), 1, false);
        
        std::cout << "Query processed: " << query_id << " (" << count << " results)" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error processing query: " << e.what() << std::endl;
        
        json error_response;
        error_response["query_id"] = query.value("query_id", "");
        error_response["status"] = "error";
        error_response["error"] = e.what();
        std::string payload = error_response.dump();
        mqtt_client->publish(config.query_response_topic(), payload.c_str(), payload.length(), 1, false);
    }
}

void DatabaseManager::save_log_to_mongodb(mongocxx::database& db, 
                                        const std::string& device_id,
                                        const std::string& log_level,
                                        const json& payload,
                                        const std::string& topic,
                                        const bsoncxx::document::view& device_info) {
    try {
        std::string log_code = payload.value("log_code", "UNKNOWN");
        
        auto now = std::chrono::system_clock::now();
        auto ingestion_time = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        
        // 구조화된 ID 생성
        std::string device_code = device_info["device_code"] ? std::string(device_info["device_code"].get_string().value) : "NA";
        std::string ulid = generate_ulid();
        std::string structured_id = device_code + "-" + log_code + "-" + ulid;

        // log_stream 생성
        time_t now_time_t = std::chrono::system_clock::to_time_t(now);
        char time_buf[12];
        strftime(time_buf, sizeof(time_buf), "%Y/%m/%d", std::gmtime(&now_time_t));
        std::string log_stream = device_id + "/" + time_buf + "/" + log_level;

        // Severity 계산
        std::string severity = determine_severity(log_code, payload.value("metadata", json::object()), device_info);

        // BSON 문서 빌드
        bson_builder builder;
        builder << "_id" << structured_id
                << "log_group" << (device_info["log_group"] ? std::string(device_info["log_group"].get_string().value) : "unknown_group")
                << "log_stream" << log_stream
                << "device_id" << device_id
                << "device_name" << (device_info["device_name"] ? std::string(device_info["device_name"].get_string().value) : "N/A")
                << "device_type" << (device_info["device_type"] ? std::string(device_info["device_type"].get_string().value) : "N/A")
                << "location" << (device_info["location"] ? std::string(device_info["location"].get_string().value) : "N/A")
                << "log_code" << log_code
                << "severity" << severity
                << "log_level" << log_level
                << "message" << payload.value("message", "")
                << "timestamp" << bsoncxx::types::b_int64{payload.value("timestamp", ingestion_time)}
                << "ingestion_time" << bsoncxx::types::b_int64{ingestion_time}
                << "topic" << topic;

        if (payload.contains("metadata") && payload["metadata"].is_object()) {
            builder << "metadata" << bsoncxx::from_json(payload["metadata"].dump());
        }

        auto doc_to_insert = builder.extract();

        // MongoDB에 데이터 삽입
        std::cout << "\n=== Saving Log Document ===" << std::endl;
        std::cout << "Structured ID: " << structured_id << std::endl;
        std::cout << "Device: " << device_id << " (" << device_code << ")" << std::endl;
        std::cout << "Log Code: " << log_code << " | Severity: " << severity << std::endl;
        std::cout << "Message: " << payload.value("message", "") << std::endl;
        std::cout << "Log Stream: " << log_stream << std::endl;
        
        // 그룹별 전용 컬렉션에 삽입
        if (device_info["log_group"]) {
            std::string group_str = std::string(device_info["log_group"].get_string().value);
            std::replace(group_str.begin(), group_str.end(), '/', '_');
            std::replace(group_str.begin(), group_str.end(), '-', '_');
            if (!group_str.empty() && group_str.front() == '_') {
                group_str.erase(0, 1);
            }
            std::string group_collection_name = "logs_" + group_str;
            db[group_collection_name].insert_one(doc_to_insert.view());
            std::cout << "✓ Saved to group collection: " << group_collection_name << std::endl;
        }

        // logs_all 컬렉션에 삽입
        db[config.all_logs_collection()].insert_one(doc_to_insert.view());
        std::cout << "✓ Saved to " << config.all_logs_collection() << " collection" << std::endl;
        std::cout << "=========================\n" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error saving log to MongoDB: " << e.what() << std::endl;
    }
}