#pragma once
#include <iostream>
#include <netinet/in.h>
#include <ostream>
#include <stdexcept>
#include <string>

enum class ExceptionType {
    INVALID = 0,
    INVALID_SOCKET = 1,
};

class Exception : public std::runtime_error {
  public:
    explicit Exception(const std::string &message)
        : std::runtime_error(message), type_(ExceptionType::INVALID) {
        std::cerr << "Message : " << message << std::endl;
    }
    Exception(ExceptionType type, const std::string &message)
        : std::runtime_error(message), type_(type) {
        std::cerr << "Exception Type : " << ExceptionTypeToString(type_) << std::endl
                  << "Message : " << message << std::endl;
    }
    static std::string ExceptionTypeToString(ExceptionType type) {
        switch (type) {
        case ExceptionType::INVALID:
            return "Invalid";
        case ExceptionType::INVALID_SOCKET:
            return "Invalid socket";
        default:
            return "Unknown";
        }
    }

  private:
    ExceptionType type_;
};