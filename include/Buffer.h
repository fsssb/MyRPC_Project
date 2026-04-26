#ifndef MYRPCPROJECT_INCLUDE_BUFFER_H_
#define MYRPCPROJECT_INCLUDE_BUFFER_H_

#include <cstddef>
#include <string>
#include <vector>

class Buffer {
public:
    explicit Buffer(std::size_t initialSize = 1024);
    virtual ~Buffer() = default;

    std::size_t readableBytes() const;
    std::size_t writableBytes() const;

    const char* peek() const;

    void retrieve(std::size_t len);
    void retrieveAll();
    void reset();
    std::string retrieveAsString(std::size_t len);
    std::string retrieveAllAsString();

    void append(const char* data, std::size_t len);
    void append(const std::string& data);
    void append(std::string&& data);

private:
    void ensureWritableBytes(std::size_t len);

private:
    std::vector<char> buffer_;
    std::size_t readIndex_{0};
    std::size_t writeIndex_{0};
};

#endif  // MYRPCPROJECT_INCLUDE_BUFFER_H_
