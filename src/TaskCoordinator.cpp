#include "TaskCoordinator.h"

#include <chrono>
#include <future>
#include <thread>

std::string TaskCoordinator::execute(const std::string& prompt) const {
    auto futureA = std::async(std::launch::async, [this, prompt]() {
        return runSubTaskA(prompt);
    });
    auto futureB = std::async(std::launch::async, [this, prompt]() {
        return runSubTaskB(prompt);
    });

    const std::string partA = futureA.get();
    const std::string partB = futureB.get();
    return "AI: " + partA + " | " + partB;
}

std::string TaskCoordinator::runSubTaskA(const std::string& prompt) const {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    return "Intent(" + prompt + ")";
}

std::string TaskCoordinator::runSubTaskB(const std::string& prompt) const {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    return "Reasoning(" + prompt + ")";
}
