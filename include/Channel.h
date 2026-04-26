#ifndef MYRPCPROJECT_INCLUDE_CHANNEL_H_
#define MYRPCPROJECT_INCLUDE_CHANNEL_H_

#include <functional>
#include <memory>

class EventLoop;

class Channel {
public:
    using EventCallback = std::function<void()>;

    Channel(EventLoop* loop, int fd);
    virtual ~Channel() = default;

    void handleEvent() const;

    void setReadCallback(EventCallback cb);
    void setWriteCallback(EventCallback cb);
    void setCloseCallback(EventCallback cb);
    void setErrorCallback(EventCallback cb);

    int fd() const;
    short events() const;
    void setRevents(short revents);
    bool isNoneEvent() const;
    bool isWriting() const;

    void enableReading();
    void enableWriting();
    void disableWriting();
    void disableAll();

private:
    void update();

private:
    EventLoop* loop_;
    const int fd_;
    short events_{0};
    short revents_{0};

    EventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};

#endif  // MYRPCPROJECT_INCLUDE_CHANNEL_H_
