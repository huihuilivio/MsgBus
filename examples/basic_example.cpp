#include "msgbus/message_bus.h"

#include <iostream>
#include <string>
#include <thread>
#include <chrono>

struct SensorData {
    int sensor_id;
    double value;
};

struct LogEvent {
    std::string level;
    std::string message;
};

int main() {
    msgbus::MessageBus bus;
    bus.start();

    // --- Subscribe to sensor data ---
    auto sub1 = bus.subscribe<SensorData>("sensor/temperature",
        [](const SensorData& data) {
            std::cout << "[Subscriber 1] Sensor " << data.sensor_id
                      << " temperature: " << data.value << "\n";
        });

    auto sub2 = bus.subscribe<SensorData>("sensor/temperature",
        [](const SensorData& data) {
            if (data.value > 50.0) {
                std::cout << "[Subscriber 2] WARNING: High temperature "
                          << data.value << " from sensor " << data.sensor_id
                          << "\n";
            }
        });

    // --- Subscribe to log events ---
    auto sub3 = bus.subscribe<LogEvent>("system/log",
        [](const LogEvent& event) {
            std::cout << "[Logger] [" << event.level << "] "
                      << event.message << "\n";
        });

    // --- Publish some messages ---
    bus.publish<SensorData>("sensor/temperature", {1, 23.5});
    bus.publish<SensorData>("sensor/temperature", {2, 67.8});
    bus.publish<LogEvent>("system/log", {"INFO", "System started"});
    bus.publish<SensorData>("sensor/temperature", {1, 42.1});
    bus.publish<LogEvent>("system/log", {"ERROR", "Disk full"});

    // Wait for dispatcher to process
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // --- Unsubscribe sub1 ---
    std::cout << "\n--- Unsubscribing subscriber 1 ---\n\n";
    bus.unsubscribe(sub1);

    bus.publish<SensorData>("sensor/temperature", {3, 55.0});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    bus.stop();
    std::cout << "\nDone.\n";
    return 0;
}
