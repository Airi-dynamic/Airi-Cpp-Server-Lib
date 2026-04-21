#include "log/LogFile.h"
#include <cassert>
#include <cstring>
#include <ctime>

LogFile::LogFile(const std::string &basename, size_t rollSizeBytes)
    : basename_(basename), rollSizeBytes_(rollSizeBytes), writtenBytes_(0), fp_(nullptr) {
    rollFile(); // 构造时立即创建第一个日志文件
}

LogFile::~LogFile() {
    if (fp_) {
        fflush(fp_);
        fclose(fp_);
    }
}

std::string LogFile::getFileName() const {
    char timebuf[32];
    std::time_t now = std::time(nullptr);
    std::tm *tm = std::localtime(&now);
    // 格式：basename.YYYYMMDD_HHMMSS.log
    strftime(timebuf, sizeof(timebuf), "%Y%m%d_%H%M%S", tm);
    return basename_ + '.' + timebuf + ".log";
}

void LogFile::rollFile() {
    if (fp_) {
        fflush(fp_);
        fclose(fp_);
    }
    std::string filename = getFileName();
    fp_ = fopen(filename.c_str(), "ae"); // 'e' = O_CLOEXEC
    if (!fp_) {
        // 退路：写入 stderr，不 abort（避免日志系统崩溃拖垮业务）
        fprintf(stderr, "[LogFile] Failed to open log file: %s\n", filename.c_str());
        fp_ = stderr;
    }
    writtenBytes_ = 0;
}

void LogFile::append(const char *data, int len) {
    if (writtenBytes_ + static_cast<size_t>(len) > rollSizeBytes_) {
        rollFile();
    }
    size_t written = fwrite(data, 1, static_cast<size_t>(len), fp_);
    writtenBytes_ += written;
}

void LogFile::flush() {
    if (fp_)
        fflush(fp_);
}
