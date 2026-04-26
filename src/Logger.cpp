#include "Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::Logger() : running_(true) {
    fileStream_.open("rpc.log", std::ios::out | std::ios::app);
    worker_ = std::make_unique<std::thread>(&Logger::workerLoop, this);
}

Logger::~Logger() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    cv_.notify_all();
    if (worker_ && worker_->joinable()) {
        worker_->join();
    }
    if (fileStream_.is_open()) {
        fileStream_.flush();
        fileStream_.close();
    }
}

void Logger::log(Level level, const char* file, int line, const std::string& message) {
    auto now = std::chrono::system_clock::now();
    std::time_t timePoint = std::chrono::system_clock::to_time_t(now);

    std::tm tmSnapshot {};
#if defined(__APPLE__) || defined(__linux__)
    localtime_r(&timePoint, &tmSnapshot);
#else
    localtime_s(&tmSnapshot, &timePoint);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tmSnapshot, "%Y-%m-%d %H:%M:%S")
        << " [" << levelToString(level) << "] "
        << file << ":" << line << " - "
        << message;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(oss.str());
    }
    cv_.notify_one();
}

void Logger::workerLoop() {
    while (true) {
        std::deque<std::string> localQueue;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return !queue_.empty() || !running_;
            });

            if (queue_.empty() && !running_) {
                break;
            }
            localQueue.swap(queue_);
        }

        for (const auto& line : localQueue) {
            if (fileStream_.is_open()) {
                fileStream_ << line << '\n';
            }
        }
        if (fileStream_.is_open()) {
            fileStream_.flush();
        }
    }

    std::deque<std::string> remaining;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        remaining.swap(queue_);
    }
    for (const auto& line : remaining) {
        if (fileStream_.is_open()) {
            fileStream_ << line << '\n';
        }
    }
}

const char* Logger::levelToString(Level level) {
    switch (level) {
        case Level::Info:
            return "INFO";
        case Level::Error:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}
