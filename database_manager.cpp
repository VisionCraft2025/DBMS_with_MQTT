#include "database_manager.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <random>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/uri.hpp>
#include <regex>

using bson_builder = bsoncxx::builder::stream::document;
using bsoncxx::builder::stream::finalize;

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

void DatabaseManager::process_statistics_request(mongocxx::client& mongo_client,
                                                 mqtt::async_client* mqtt_client,
                                                 const json& request) {
    std::cout << "Processing statistics request: " << request.dump() << std::endl;
    
    // 디바이스 ID 확인
    std::string device_id = request.value("device_id", "");
    if (device_id.empty()) {
        std::cerr << "Statistics request error: device_id is missing" << std::endl;
        return;
    }

    // 요청 ID 확인 (중복 요청 방지)
    static std::string last_request_id = "";
    std::string request_id = request.value("request_id", "");
    
    if (!request_id.empty() && request_id == last_request_id) {
        std::cout << "Duplicate request detected (ID: " << request_id << "). Ignoring." << std::endl;
        return;
    }
    
    // 새 요청 ID 저장
    if (!request_id.empty()) {
        last_request_id = request_id;
    }

    try {
        auto db = mongo_client[config.mongo_db_name()];
        auto collection = db[config.all_logs_collection()];

        // 시간 범위 설정
        int64_t start_time = 0, end_time = 0;
        if (request.contains("time_range") && 
            request["time_range"].contains("start") && 
            request["time_range"].contains("end")) {
            
            start_time = request["time_range"]["start"];
            end_time = request["time_range"]["end"];
            std::cout << "Using time range from request: " << start_time << " to " << end_time << std::endl;
        } else {
            // 기본값: 최근 24시간
            auto now = std::chrono::system_clock::now();
            end_time = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            start_time = end_time - (24 * 60 * 60 * 1000); // 24시간 전
            std::cout << "Using default time range: " << start_time << " to " << end_time << std::endl;
        }

        // 통계 계산 및 응답 전송 함수
        auto calculate_and_publish = [&](const std::string& dev_id) {
            // SPD 로그 필터링
            bson_builder filter_builder;
            filter_builder << "device_id" << dev_id
                          << "log_code" << "SPD"
                          << "timestamp" << bsoncxx::builder::stream::open_document
                          << "$gte" << bsoncxx::types::b_int64{start_time}
                          << "$lte" << bsoncxx::types::b_int64{end_time}
                          << bsoncxx::builder::stream::close_document;

            // 먼저 SPD 로그가 있는지 확인
            auto count = collection.count_documents(filter_builder.view());
            std::cout << "Found " << count << " SPD logs for device " << dev_id << std::endl;
            
            if (count == 0) {
                std::cout << "No SPD logs found for device " << dev_id << ". Checking for any logs..." << std::endl;
                
                // 디바이스 자체가 존재하는지 확인
                bson_builder device_filter;
                device_filter << "device_id" << dev_id;
                auto device_count = collection.count_documents(device_filter.view());
                
                if (device_count == 0) {
                    std::cout << "No logs found for device " << dev_id << ". Device might not exist." << std::endl;
                } else {
                    std::cout << "Device " << dev_id << " exists with " << device_count << " logs, but no SPD logs." << std::endl;
                }
            }

            // 평균 속도 계산 (MongoDB Aggregation 사용)
            mongocxx::pipeline pipeline{};
            pipeline.match(filter_builder.view());
            
            // 메시지 필드 확인 (디버깅)
            auto sample_doc = collection.find_one(filter_builder.view());
            if (sample_doc) {
                auto doc_view = sample_doc->view();
                if (doc_view["message"]) {
                    std::string message_str(doc_view["message"].get_string().value);
                    std::cout << "Sample SPD message: '" << message_str << "'" << std::endl;
                }
            }
            
            // 숫자 형식의 메시지만 필터링 (정규식 사용)
            pipeline.match(bson_builder{} << "message" << bsoncxx::builder::stream::open_document
                                        << "$regex" << "^[0-9]+$"
                                        << bsoncxx::builder::stream::close_document
                                        << finalize);
            
            // 문자열을 숫자로 변환
            pipeline.add_fields(bson_builder{} << "speed_value" << bsoncxx::builder::stream::open_document
                                             << "$toDouble" << "$message"
                                             << bsoncxx::builder::stream::close_document
                                             << finalize);

            // 0보다 큰 값만 필터링 (속도가 0인 경우 평균 계산에서 제외)
            pipeline.match(bson_builder{} << "speed_value" << bsoncxx::builder::stream::open_document
                                        << "$gt" << 0.0
                                        << bsoncxx::builder::stream::close_document
                                        << finalize);
                                        
            // 평균 계산
            pipeline.group(bson_builder{} << "_id" << bsoncxx::types::b_null{}
                                        << "average" << bsoncxx::builder::stream::open_document
                                        << "$avg" << "$speed_value"
                                        << bsoncxx::builder::stream::close_document
                                        << finalize);

            // 평균 속도 추출
            double average_speed = 0.0;
            auto cursor = collection.aggregate(pipeline);
            bool has_results = false;
            
            for (auto&& doc : cursor) {
                has_results = true;
                if (doc["average"] && doc["average"].type() == bsoncxx::type::k_double) {
                    average_speed = doc["average"].get_double();
                    std::cout << "Calculated average speed: " << average_speed << std::endl;
                    break;
                }
            }
            
            if (!has_results) {
                std::cout << "No valid numeric SPD logs found for average calculation" << std::endl;
            }

            // 현재 속도 조회 (최신 SPD 로그)
            int current_speed = 0;
            mongocxx::options::find opts{};
            opts.sort(bson_builder{} << "timestamp" << -1 << finalize); // 최신순 정렬
            
            // 숫자 형식의 메시지만 필터링 (새로운 필터 생성)
            bson_builder number_filter;
            number_filter << "device_id" << dev_id
                         << "log_code" << "SPD"
                         << "timestamp" << bsoncxx::builder::stream::open_document
                         << "$gte" << bsoncxx::types::b_int64{start_time}
                         << "$lte" << bsoncxx::types::b_int64{end_time}
                         << bsoncxx::builder::stream::close_document
                         << "message" << bsoncxx::builder::stream::open_document
                         << "$regex" << "^[0-9]+$"
                         << bsoncxx::builder::stream::close_document;
            
            opts.limit(1); // 가장 최근 1개만

            auto latest_doc = collection.find_one(number_filter.view(), opts);
            if (latest_doc) {
                try {
                    auto doc_view = latest_doc->view();
                    if (doc_view["message"]) {
                        std::string message_str(doc_view["message"].get_string().value);
                        current_speed = std::stoi(message_str);
                        std::cout << "Current speed from latest log: " << current_speed << std::endl;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Error parsing current_speed: " << e.what() << std::endl;
                }
            } else {
                std::cout << "No valid numeric SPD logs found for current speed" << std::endl;
            }

            // 응답 생성
            json response;
            response["device_id"] = dev_id;
            response["average"] = static_cast<int>(average_speed); // 정수로 변환
            response["current_speed"] = current_speed;
            if (!request_id.empty()) {
                response["request_id"] = request_id;
            }

            // 응답 전송
            std::string response_topic = "factory/" + dev_id + "/msg/statistics";
            std::string payload = response.dump();
            mqtt_client->publish(response_topic, payload.c_str(), payload.length(), 1, false);
            
            std::cout << "Published statistics for " << dev_id << ": " << payload << std::endl;
        };

        // 디바이스 ID에 따라 처리
        if (device_id == "All") {
            // 모든 디바이스 ID 조회 (SPD 로그가 있는 디바이스)
            mongocxx::pipeline distinct_pipeline{};
            distinct_pipeline.match(bson_builder{} << "log_code" << "SPD" << finalize);
            distinct_pipeline.group(bson_builder{} << "_id" << "$device_id" << finalize);
            
            auto distinct_cursor = collection.aggregate(distinct_pipeline);
            int device_count = 0;
            
            for (auto&& doc : distinct_cursor) {
                if (doc["_id"] && doc["_id"].type() == bsoncxx::type::k_string) {
                    std::string id_str(doc["_id"].get_string().value);
                    calculate_and_publish(id_str);
                    device_count++;
                }
            }
            
            std::cout << "Processed statistics for " << device_count << " devices" << std::endl;
        } else {
            // 단일 디바이스 처리
            calculate_and_publish(device_id);
        }

    } catch (const std::exception& e) {
        std::cerr << "Error processing statistics request: " << e.what() << std::endl;
        
        // 에러 응답 전송
        if (device_id != "All") {
            json error_response;
            error_response["device_id"] = device_id;
            error_response["error"] = e.what();
            if (!request_id.empty()) {
                error_response["request_id"] = request_id;
            }
            
            std::string response_topic = "factory/" + device_id + "/msg/statistics";
            std::string payload = error_response.dump();
            mqtt_client->publish(response_topic, payload.c_str(), payload.length(), 1, false);
        }
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

void DatabaseManager::save_statistics_to_mongodb(mongocxx::database& db,
                                                const std::string& device_id,
                                                const json& payload) {
    try {
        std::cout << "\n=========================" << std::endl;
        std::cout << "Saving statistics data for device: " << device_id << std::endl;
        
        // 현재 시간 생성
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        
        // message 객체에서 통계 데이터 추출
        json message = payload["message"];
        json time_range = payload["time_range"];
        
        // BSON 문서 생성
        bson_builder doc{};
        doc << "_id" << generate_ulid()
            << "device_id" << device_id
            << "log_code" << payload.value("log_code", "")
            << "statistics" << bsoncxx::builder::stream::open_document
                << "total" << message.value("total", "")
                << "pass" << message.value("pass", "")
                << "fail" << message.value("fail", "")
                << "failure" << message.value("failure", "")
            << bsoncxx::builder::stream::close_document
            << "time_range" << bsoncxx::builder::stream::open_document
                << "start" << time_range.value("start", 0)
                << "end" << time_range.value("end", 0)
            << bsoncxx::builder::stream::close_document
            << "created_at" << bsoncxx::types::b_date{std::chrono::milliseconds{timestamp}};
        
        auto doc_value = doc << finalize;
        
        // statistics 컬렉션에 저장
        db[config.statistics_collection()].insert_one(doc_value.view());
        
        std::cout << "✓ Statistics saved to " << config.statistics_collection() << " collection" << std::endl;
        std::cout << "  - Total: " << message.value("total", "") << std::endl;
        std::cout << "  - Pass: " << message.value("pass", "") << std::endl;
        std::cout << "  - Fail: " << message.value("fail", "") << std::endl;
        std::cout << "  - Failure rate: " << message.value("failure", "") << std::endl;
        std::cout << "=========================\n" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error saving statistics to MongoDB: " << e.what() << std::endl;
    }
}

void DatabaseManager::process_statistics_data_request(mongocxx::client& mongo_client,
                                                     mqtt::async_client* mqtt_client,
                                                     const std::string& device_id,
                                                     const std::string& response_topic) {
    try {
        std::cout << "\n=========================" << std::endl;
        std::cout << "Processing statistics data request for device: " << device_id << std::endl;
        
        auto db = mongo_client[config.mongo_db_name()];
        auto collection = db[config.statistics_collection()];
        
        // 해당 디바이스의 가장 최근 통계 데이터 조회
        mongocxx::options::find opts{};
        opts.sort(bsoncxx::builder::stream::document{} << "created_at" << -1 << finalize);
        opts.limit(1);
        
        auto filter = bsoncxx::builder::stream::document{} 
            << "device_id" << device_id 
            << finalize;
        
        auto cursor = collection.find(filter.view(), opts);
        
        json response;
        response["device_id"] = device_id;
        response["status"] = "success";
        
        if (cursor.begin() != cursor.end()) {
            auto doc = *cursor.begin();
            
            // BSON에서 JSON으로 변환
            std::string bson_json = bsoncxx::to_json(doc);
            json bson_data = json::parse(bson_json);
            
            // 응답 데이터 구성
            response["data"] = {
                {"log_code", bson_data["log_code"]},
                {"message", bson_data["statistics"]},
                {"time_range", bson_data["time_range"]}
            };
            
            std::cout << "✓ Found statistics data for device: " << device_id << std::endl;
        } else {
            response["status"] = "not_found";
            response["message"] = "No statistics data found for device: " + device_id;
            std::cout << "✗ No statistics data found for device: " << device_id << std::endl;
        }
        
        // MQTT로 응답 전송
        if (mqtt_client) {
            std::string response_str = response.dump();
            auto msg = mqtt::make_message(response_topic, response_str);
            msg->set_qos(1);
            mqtt_client->publish(msg);
            std::cout << "✓ Response sent to topic: " << response_topic << std::endl;
        }
        
        std::cout << "=========================\n" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error processing statistics data request: " << e.what() << std::endl;
        
        // 에러 응답 전송
        if (mqtt_client) {
            json error_response;
            error_response["device_id"] = device_id;
            error_response["status"] = "error";
            error_response["message"] = e.what();
            
            std::string response_str = error_response.dump();
            auto msg = mqtt::make_message(response_topic, response_str);
            msg->set_qos(1);
            mqtt_client->publish(msg);
        }
    }
}