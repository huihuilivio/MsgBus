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

// Coroutine that waits for a message on a topic
Task waitForChat(msgbus::MessageBus& bus) {
    std::cout << "[Coroutine] Waiting for chat message...\n";
    auto msg = co_await bus.async_wait<ChatMessage>("chat/room1");
    std::cout << "[Coroutine] Received: " << msg.user << " says: "
              << msg.text << "\n";
}

// Coroutine that waits for an int on a topic
Task waitForSignal(msgbus::MessageBus& bus) {
    std::cout << "[Coroutine] Waiting for shutdown signal...\n";
    auto code = co_await bus.async_wait<int>("system/shutdown");
    std::cout << "[Coroutine] Shutdown signal received, code=" << code << "\n";
}

int main() {
    msgbus::MessageBus bus;
    bus.start();

    // Launch coroutines (they start immediately and suspend at co_await)
    waitForChat(bus);
    waitForSignal(bus);

    // Simulate some delay, then publish
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::cout << "[Main] Sending chat message...\n";
    bus.publish<ChatMessage>("chat/room1", {"Alice", "Hello everyone!"});

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::cout << "[Main] Sending shutdown signal...\n";
    bus.publish<int>("system/shutdown", 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    bus.stop();
    std::cout << "\nDone.\n";
    return 0;
}
