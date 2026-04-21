#ifndef UTIL_H
#define UTIL_H

// 历史兼容工具：仅供旧示例路径使用。
// 核心库代码应优先使用“返回值 + 日志 + 上层决策”的错误语义。
void ErrIf(bool, const char *);

#endif