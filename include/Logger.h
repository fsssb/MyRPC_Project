#ifndef MYRPCPROJECT_INCLUDE_LOGGER_H_
#define MYRPCPROJECT_INCLUDE_LOGGER_H_

#include <condition_variable>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class Logger {
public:
    enum class Level {
        Info,
        Error
    };

    static Logger& instance();

    void log(Level level, const char* file, int line, const std::string& message);

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

private:
    Logger();
    virtual ~Logger();

    void workerLoop();
    static const char* levelToString(Level level);

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::ofstream fileStream_;
    std::unique_ptr<std::thread> worker_;
    bool running_{false};
};

#define LOG_INFO(msg) Logger::instance().log(Logger::Level::Info, __FILE__, __LINE__, (msg))
#define LOG_ERROR(msg) Logger::instance().log(Logger::Level::Error, __FILE__, __LINE__, (msg))

#endif  // MYRPCPROJECT_INCLUDE_LOGGER_H_
