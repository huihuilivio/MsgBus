#include "msgbus/message_bus.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

struct SensorData {
    int sensor_id;
    double value;
};

struct LogEvent {
    std::string level;
    std::string message;
};

int main() {
    // Multi-dispatcher: 1 router thread + 4 worker threads (topic hash sharding).
    // Use 0 for auto (= hardware_concurrency).
    msgbus::MessageBus bus(msgbus::kDefaultQueueCapacity, 4);
    bus.start();

    // --- Exact topic subscriptions ---
    auto sub1 = bus.subscribe<SensorData>("sensor/temperature",
        [](const SensorData& data) {
            std::cout << "[Temp] Sensor " << data.sensor_id
                      << " = " << data.value << "°C\n";
        });

    auto sub2 = bus.subscribe<SensorData>("sensor/humidity",
        [](const SensorData& data) {
            std::cout << "[Humidity] Sensor " << data.sensor_id
                      << " = " << data.value << "%\n";
        });

    // --- Wildcard subscription: '*' matches one level ---
    // Receives ALL sensor types (temperature, humidity, pressure, ...)
    auto sub_all_sensors = bus.subscribe<SensorData>("sensor/*",
        [](const SensorData& data) {
            std::cout << "[AllSensors] Sensor " << data.sensor_id
                      << " value: " << data.value << "\n";
        });

    // --- Wildcard subscription: '#' matches zero or more trailing levels ---
    // Receives ALL log events under system/ (system/log, system/log/audit, ...)
    auto sub_system = bus.subscribe<LogEvent>("system/#",
        [](const LogEvent& event) {
            std::cout << "[System] [" << event.level << "] "
                      << event.message << "\n";
        });

    // --- Publish messages to various topics ---
    bus.publish<SensorData>("sensor/temperature", {1, 23.5});
    bus.publish<SensorData>("sensor/humidity",    {2, 65.3});
    bus.publish<SensorData>("sensor/temperature", {3, 67.8});
    bus.publish<SensorData>("sensor/pressure",    {4, 1013.25});
    bus.publish<LogEvent>("system/log",           {"INFO", "System started"});
    bus.publish<LogEvent>("system/log/audit",     {"WARN", "Config changed"});
    bus.publish<LogEvent>("system/log",           {"ERROR", "Disk full"});

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // --- Unsubscribe the wildcard sensor listener ---
    std::cout << "\n--- Unsubscribing wildcard sensor listener ---\n\n";
    bus.unsubscribe(sub_all_sensors);

    bus.publish<SensorData>("sensor/temperature", {5, 55.0});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    bus.stop();
    std::cout << "\nDone.\n";
    return 0;
}
