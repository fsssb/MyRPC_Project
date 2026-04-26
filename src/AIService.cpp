#include "AIService.h"
#include "TaskCoordinator.h"

#include <utility>

AIService::AIService(std::size_t workerCount) {
    if (workerCount == 0) {
        workerCount = 1;
    }
    coordinator_ = std::make_unique<TaskCoordinator>();
    workers_.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i) {
        workers_.emplace_back([this]() {
            workerLoop();
        });
    }
}

AIService::~AIService() {
    stop();
}

void AIService::inferAsync(std::string prompt, DoneCallback cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        tasks_.push(Task{std::move(prompt), std::move(cb)});
    }
    cv_.notify_one();
}

void AIService::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

void AIService::workerLoop() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return !running_ || !tasks_.empty();
            });

            if (!running_ && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        if (task.done) {
            task.done(coordinator_->execute(task.prompt));
        }
    }
}
