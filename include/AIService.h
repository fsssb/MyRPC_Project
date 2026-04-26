#ifndef MYRPCPROJECT_INCLUDE_AISERVICE_H_
#define MYRPCPROJECT_INCLUDE_AISERVICE_H_

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

class AIService {
public:
    using DoneCallback = std::function<void(std::string)>;

    explicit AIService(std::size_t workerCount = 2);
    virtual ~AIService();

    AIService(const AIService&) = delete;
    AIService& operator=(const AIService&) = delete;

    void inferAsync(std::string prompt, DoneCallback cb);
    void stop();

private:
    struct Task {
        std::string prompt;
        DoneCallback done;
    };

    void workerLoop();

private:
    std::unique_ptr<class TaskCoordinator> coordinator_;
    std::vector<std::thread> workers_;
    std::queue<Task> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_{true};
};

#endif  // MYRPCPROJECT_INCLUDE_AISERVICE_H_
