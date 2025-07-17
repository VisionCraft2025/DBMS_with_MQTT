# CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(factory_monitor_db_writer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH};/home/kwon/dev/cpp_libs/mongo-cxx-driver;/home/kwon/dev/cpp_libs/paho.mqtt.cpp")
include_directories(/home/kwon/dev/cpp_libs/paho.mqtt.cpp/include)
link_directories(/home/kwon/dev/cpp_libs/paho.mqtt.cpp/build/src)
find_package(mongocxx REQUIRED)
find_package(PahoMqttCpp REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)

# 소스 파일들
set(SOURCES
    main.cpp
    database_manager.cpp
    mqtt_handler.cpp
)

# 실행 파일 정의
add_executable(db_mqtt ${SOURCES})

target_link_libraries(db_mqtt PRIVATE
    mongo::mongocxx_shared
    paho-mqttpp3
    paho-mqtt3as
    nlohmann_json::nlohmann_json
)