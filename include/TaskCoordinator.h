#ifndef MYRPCPROJECT_INCLUDE_TASKCOORDINATOR_H_
#define MYRPCPROJECT_INCLUDE_TASKCOORDINATOR_H_

#include <string>

class TaskCoordinator {
public:
    TaskCoordinator() = default;
    virtual ~TaskCoordinator() = default;

    std::string execute(const std::string& prompt) const;

private:
    std::string runSubTaskA(const std::string& prompt) const;
    std::string runSubTaskB(const std::string& prompt) const;
};

#endif  // MYRPCPROJECT_INCLUDE_TASKCOORDINATOR_H_
