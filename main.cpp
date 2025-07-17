#include <iostream>
#include <thread>
#include <chrono>
#include <mongocxx/instance.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/uri.hpp>
#include <mqtt/async_client.h>
#include "config.h"
#include "database_manager.h"
#include "mqtt_handler.h"

int main(int argc, char* argv[]) {
    // MongoDB 인스턴스 초기화 (프로그램 시작 시 한 번만)
    mongocxx::instance instance{};
    
    // 설정 로드
    Config config;
    
    std::cout << "Connecting to MQTT broker at " << config.mqtt_server_address() << "..." << std::endl;
    mqtt::async_client client(config.mqtt_server_address(), config.mqtt_client_id());

    std::cout << "Connecting to MongoDB at " << config.mongo_uri() << "..." << std::endl;
    mongocxx::client mongo_client{mongocxx::uri{config.mongo_uri()}};

    // 데이터베이스 매니저 생성
    DatabaseManager db_manager(config);
    
    // MQTT 핸들러 생성 및 설정
    MqttHandler mqtt_handler(mongo_client, &client, config, db_manager);
    client.set_callback(mqtt_handler);

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

    // 프로그램이 종료되지 않도록 대기
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}