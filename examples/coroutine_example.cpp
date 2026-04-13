#include "msgbus/message_bus.h"

#include <chrono>
#include <coroutine>
#include <iostream>
#include <string>
#include <thread>

// Simple fire-and-forget coroutine task type
struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

struct ChatMessage {
    std::string user;
    std::string text;
};

struct Alert {
    int code;
    std::string detail;
};

// Coroutine: wait for a specific chat room message
Task waitForChat(msgbus::MessageBus& bus) {
    std::cout << "[Coroutine] Waiting for chat/room1 ...\n";
    auto msg = co_await bus.async_wait<ChatMessage>("chat/room1");
    std::cout << "[Coroutine] Received: " << msg.user << " says: "
              << msg.text << "\n";
}

// Coroutine: wait for ANY alert using wildcard '#'
Task waitForAlert(msgbus::MessageBus& bus) {
    std::cout << "[Coroutine] Waiting for alert/# (any alert) ...\n";
    auto a = co_await bus.async_wait<Alert>("alert/#");
    std::cout << "[Coroutine] Alert code=" << a.code
              << " detail: " << a.detail << "\n";
}

// Coroutine: wait for shutdown signal
Task waitForSignal(msgbus::MessageBus& bus) {
    std::cout << "[Coroutine] Waiting for system/shutdown ...\n";
    auto code = co_await bus.async_wait<int>("system/shutdown");
    std::cout << "[Coroutine] Shutdown signal received, code=" << code << "\n";
}

int main() {
    // Multi-dispatcher with 2 worker threads
    msgbus::MessageBus bus(msgbus::kDefaultQueueCapacity, 2);
    bus.start();

    // Launch coroutines (start immediately, suspend at co_await)
    waitForChat(bus);
    waitForAlert(bus);
    waitForSignal(bus);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Publish a chat message — resumes waitForChat
    std::cout << "[Main] Sending chat message...\n";
    bus.publish<ChatMessage>("chat/room1", {"Alice", "Hello everyone!"});

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Publish an alert on a nested topic — resumes waitForAlert via '#' wildcard
    std::cout << "[Main] Sending alert...\n";
    bus.publish<Alert>("alert/disk/sda1", {101, "Disk usage > 90%"});

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Publish shutdown signal
    std::cout << "[Main] Sending shutdown signal...\n";
    bus.publish<int>("system/shutdown", 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    bus.stop();
    std::cout << "\nDone.\n";
    return 0;
}
