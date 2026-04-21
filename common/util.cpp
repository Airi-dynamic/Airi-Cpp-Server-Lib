#include "util.h"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>

// 保留该函数是为了兼容旧示例代码，避免一次性破坏历史学习路径。
// 新增或重构代码应避免继续引入该退出式错误处理。
void ErrIf(bool condition, const char *message) {
    if (condition) {
        perror(message);
        exit(EXIT_FAILURE);
    }
}