#include "msgbus/message_bus.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

struct Event {
    std::string source;
    std::string detail;
};

int main() {
    msgbus::MessageBus bus;
    bus.start();

    // '*' matches exactly one level
    // Matches: home/living_room, home/kitchen, but NOT home/floor1/room2
    auto sub_star = bus.subscribe<Event>("home/*",
        [](const Event& e) {
            std::cout << "[home/*]  source=" << e.source
                      << "  detail=" << e.detail << "\n";
        });

    // '#' matches zero or more trailing levels
    // Matches: home, home/kitchen, home/floor1/room2, ...
    auto sub_hash = bus.subscribe<Event>("home/#",
        [](const Event& e) {
            std::cout << "[home/#]  source=" << e.source
                      << "  detail=" << e.detail << "\n";
        });

    // Exact match on a specific room
    auto sub_exact = bus.subscribe<Event>("home/kitchen",
        [](const Event& e) {
            std::cout << "[home/kitchen]  source=" << e.source
                      << "  detail=" << e.detail << "\n";
        });

    std::cout << "--- Publish to home/kitchen ---\n";
    // Matched by: home/* (one level), home/# (multi-level), home/kitchen (exact)
    bus.publish<Event>("home/kitchen", {"oven", "preheated to 200°C"});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "\n--- Publish to home/living_room ---\n";
    // Matched by: home/* and home/#, but NOT home/kitchen
    bus.publish<Event>("home/living_room", {"thermostat", "set to 22°C"});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "\n--- Publish to home/floor1/room2 ---\n";
    // Matched by: home/# only (two levels deep, so '*' doesn't match)
    bus.publish<Event>("home/floor1/room2", {"light", "turned on"});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "\n--- Publish to home (root level) ---\n";
    // Matched by: home/# only ('#' matches zero trailing levels)
    bus.publish<Event>("home", {"gateway", "rebooted"});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    bus.stop();
    std::cout << "\nDone.\n";
    return 0;
}
