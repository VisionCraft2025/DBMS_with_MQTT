#include <iostream>
#include <string>
#include <vector>
#include <regex>
#include <chrono>
#include <random>
#include <optional>
#include <thread>
#include <algorithm>

// 3rd party libraries
#include <mqtt/async_client.h>
#include <nlohmann/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

using json = nlohmann::json;
using bson_builder = bsoncxx::builder::stream::document;

// --- 설정값 (README.md 기반) ---
const std::string MQTT_SERVER_ADDRESS = "tcp://mqtt.kwon.pics:1883";
const std::string MQTT_CLIENT_ID = "factory_monitor_db_writer_" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
const std::string MQTT_TOPIC = "factory/#";
const std::string QUERY_REQUEST_TOPIC = "factory/query/logs/request";
const std::string QUERY_RESPONSE_TOPIC = "factory/query/logs/response";

const std::string MONGO_URI = "mongodb://localhost:27017";
const std::string MONGO_DB_NAME = "factory_monitoring";
const std::string DEVICES_COLLECTION = "devices";
const std::string ALL_LOGS_COLLECTION = "logs_all";

// --- 유틸리티 함수 ---

// Crockford's Base32 인코딩을 사용한 간단한 ULID 생성기
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

// device_id로 devices 컬렉션에서 디바이스 정보 조회
bsoncxx::stdx::optional<bsoncxx::document::value> get_device_info(mongocxx::database& db, const std::string& device_id) {
    try {
        auto collection = db[DEVICES_COLLECTION];
        bson_builder builder;
        builder << "_id" << device_id;
        return collection.find_one(builder.view());
    } catch (const std::exception& e) {
        std::cerr << "Error finding device '" << device_id << "': " << e.what() << std::endl;
        return bsoncxx::stdx::nullopt;
    }
}

// README.md의 규칙에 따라 Severity 계산
std::string determine_severity(const std::string& log_code, const json& metadata, const bsoncxx::document::view& device_info) {
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

// 쿼리 처리 함수
void process_query_request(mongocxx::client& mongo_client, mqtt::async_client* mqtt_client, const json& query) {
    try {
        std::string query_id = query.value("query_id", "");
        std::string query_type = query.value("query_type", "");
        
        if (query_type != "logs") {
            json error_response;
            error_response["query_id"] = query_id;
            error_response["status"] = "error";
            error_response["error"] = "Unsupported query type";
            std::string payload = error_response.dump();
            mqtt_client->publish(QUERY_RESPONSE_TOPIC, payload.c_str(), payload.length(), 1, false);
            return;
        }
        
        // 새로운 MongoDB 클라이언트 생성으로 최신 데이터 보장
        mongocxx::client fresh_client{mongocxx::uri{MONGO_URI}};
        auto db = fresh_client[MONGO_DB_NAME];
        auto collection = db[ALL_LOGS_COLLECTION];
        
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
        mqtt_client->publish(QUERY_RESPONSE_TOPIC, payload.c_str(), payload.length(), 1, false);
        
        std::cout << "Query processed: " << query_id << " (" << count << " results)" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error processing query: " << e.what() << std::endl;
        
        json error_response;
        error_response["query_id"] = query.value("query_id", "");
        error_response["status"] = "error";
        error_response["error"] = e.what();
        std::string payload = error_response.dump();
        mqtt_client->publish(QUERY_RESPONSE_TOPIC, payload.c_str(), payload.length(), 1, false);
    }
}

// --- MQTT 콜백 핸들러 ---
class MqttCallbackHandler : public virtual mqtt::callback {
    mongocxx::client& m_mongo_client;
    mqtt::async_client* m_mqtt_client;

public:
    MqttCallbackHandler(mongocxx::client& client, mqtt::async_client* mqtt_client) 
        : m_mongo_client(client), m_mqtt_client(mqtt_client) {}

    void connected(const std::string& cause) override {
        std::cout << "MQTT Connected!" << std::endl;
        // 연결 성공 시 토픽 구독
        if (m_mqtt_client) {
            m_mqtt_client->subscribe(MQTT_TOPIC, 1);
            m_mqtt_client->subscribe(QUERY_REQUEST_TOPIC, 1);
            std::cout << "Subscribed to topics: " << MQTT_TOPIC << ", " << QUERY_REQUEST_TOPIC << std::endl;
        }
    }

    void connection_lost(const std::string& cause) override {
        std::cerr << "MQTT Connection lost: " << cause << std::endl;
    }

    void message_arrived(mqtt::const_message_ptr msg) override {
        try {
            std::string topic_str = msg->get_topic();
            std::cout << "Message arrived on topic: " << topic_str << std::endl;
            
            // 쿼리 요청 처리
            if (topic_str == QUERY_REQUEST_TOPIC) {
                json query = json::parse(msg->get_payload_str());
                std::cout << "Processing query request: " << query.value("query_id", "unknown") << std::endl;
                process_query_request(m_mongo_client, m_mqtt_client, query);
                return;
            }

            // 1. 토픽 파싱 (factory/{device_id}/log/{log_level}) - 쿼리 토픽이 아닌 경우만
            std::regex topic_regex("factory/([^/]+)/log/([^/]+)");
            std::smatch matches;
            if (!std::regex_match(topic_str, matches, topic_regex) || matches.size() != 3) {
                // 로그 토픽이 아닌 경우 조용히 무시 (브로커 역할 유지)
                return;
            }
            std::string device_id = matches[1].str();
            std::string log_level = matches[2].str();

            // 2. 페이로드 파싱 (JSON)
            json payload = json::parse(msg->get_payload_str());
            std::string log_code = payload.value("log_code", "UNKNOWN");

            // 3. MongoDB에서 디바이스 정보 조회
            auto db = m_mongo_client[MONGO_DB_NAME];
            auto device_info_opt = get_device_info(db, device_id);
            if (!device_info_opt) {
                std::cerr << "Device '" << device_id << "' not found in DB. Skipping." << std::endl;
                return;
            }
            auto device_info = device_info_opt->view();

            // 4. README.md 명세에 따라 로그 문서 생성
            auto now = std::chrono::system_clock::now();
            auto ingestion_time = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            
            // 4.1. 구조화된 ID 생성
            std::string device_code = device_info["device_code"] ? std::string(device_info["device_code"].get_string().value) : "NA";
            std::string ulid = generate_ulid();
            std::string structured_id = device_code + "-" + log_code + "-" + ulid;

            // 4.2. log_stream 생성
            time_t now_time_t = std::chrono::system_clock::to_time_t(now);
            char time_buf[12];
            strftime(time_buf, sizeof(time_buf), "%Y/%m/%d", std::gmtime(&now_time_t));
            std::string log_stream = device_id + "/" + time_buf + "/" + log_level;

            // 4.3. Severity 계산
            std::string severity = determine_severity(log_code, payload.value("metadata", json::object()), device_info);

            // 4.4. BSON 문서 빌드
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
                    << "topic" << msg->get_topic();

            if (payload.contains("metadata") && payload["metadata"].is_object()) {
                builder << "metadata" << bsoncxx::from_json(payload["metadata"].dump());
            }

            auto doc_to_insert = builder.extract();

            // 5. MongoDB에 데이터 삽입
            std::cout << "\n=== Saving Log Document ===" << std::endl;
            std::cout << "Structured ID: " << structured_id << std::endl;
            std::cout << "Device: " << device_id << " (" << device_code << ")" << std::endl;
            std::cout << "Log Code: " << log_code << " | Severity: " << severity << std::endl;
            std::cout << "Message: " << payload.value("message", "") << std::endl;
            std::cout << "Log Stream: " << log_stream << std::endl;
            
            // 5.1. 그룹별 전용 컬렉션에 삽입
            if (device_info["log_group"]) {
                std::string group_str = std::string(device_info["log_group"].get_string().value);
                std::replace(group_str.begin(), group_str.end(), '/', '_'); // /factory/line-a/robots -> _factory_line_a_robots
                std::replace(group_str.begin(), group_str.end(), '-', '_');
                if (!group_str.empty() && group_str.front() == '_') {
                    group_str.erase(0, 1); // 맨 앞의 '_' 제거
                }
                std::string group_collection_name = "logs_" + group_str;
                db[group_collection_name].insert_one(doc_to_insert.view());
                std::cout << "✓ Saved to group collection: " << group_collection_name << std::endl;
            }

            // 5.2. logs_all 컬렉션에 삽입
            db[ALL_LOGS_COLLECTION].insert_one(doc_to_insert.view());
            std::cout << "✓ Saved to " << ALL_LOGS_COLLECTION << " collection" << std::endl;
            std::cout << "=========================\n" << std::endl;

        } catch (const json::parse_error& e) {
            std::cerr << "JSON parse error: " << e.what() << " on topic: " << msg->get_topic() << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "An error occurred in message_arrived: " << e.what() << std::endl;
        }
    }
};

// --- 메인 함수 ---
int main(int argc, char* argv[]) {
    // MongoDB 인스턴스 초기화 (프로그램 시작 시 한 번만)
    mongocxx::instance instance{};

    std::cout << "Connecting to MQTT broker at " << MQTT_SERVER_ADDRESS << "..." << std::endl;
    mqtt::async_client client(MQTT_SERVER_ADDRESS, MQTT_CLIENT_ID);

    std::cout << "Connecting to MongoDB at " << MONGO_URI << "..." << std::endl;
    mongocxx::client mongo_client{mongocxx::uri{MONGO_URI}};

    MqttCallbackHandler cb(mongo_client, &client);
    client.set_callback(cb);

    auto connOpts = mqtt::connect_options_builder()
        .clean_session(true)
        .automatic_reconnect(std::chrono::seconds(2), std::chrono::seconds(30))
        .finalize();

    try {
        client.connect(connOpts)->wait();
        std::cout << "Connection successful. Waiting for messages..." << std::endl;
    } catch (const mqtt::exception& exc) {
        std::cerr << "Error connecting to MQTT broker: " << exc.what() << std::endl;
        return 1;
    }

    // 프로그램이 종료되지 않도록 ㅖ대기
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}