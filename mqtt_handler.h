#pragma once
#include <mqtt/async_client.h>
#include <mongocxx/client.hpp>
#include <unordered_set>
#include <memory>
#include <string>
#include <fstream>
#include "config.h"
#include "database_manager.h"

class MqttHandler : public virtual mqtt::callback {
private:
    mongocxx::client& mongo_client;
    mqtt::async_client* mqtt_client;
    const Config& config;
    DatabaseManager& db_manager;
    
    // 디바이스 상태 관리
    std::unordered_set<std::string> shutdown_devices;
    const std::string state_file = "device_states.txt";
    
    void load_device_states();
    void save_device_states();
    void set_device_shutdown(const std::string& device_id);
    void set_device_active(const std::string& device_id);
    bool is_device_shutdown(const std::string& device_id) const;

public:
    MqttHandler(mongocxx::client& client, 
                mqtt::async_client* mqtt_client, 
                const Config& cfg,
                DatabaseManager& db_mgr);

    void connected(const std::string& cause) override;
    void connection_lost(const std::string& cause) override;
    void message_arrived(mqtt::const_message_ptr msg) override;
};