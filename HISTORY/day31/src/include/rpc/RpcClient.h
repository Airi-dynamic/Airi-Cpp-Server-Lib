#pragma once
#include "rpc/RpcMessage.h"
#include <cstdint>
#include <string>

class RpcClient {
  public:
    RpcClient(const std::string &serverIp, uint16_t serverPort, int timeoutMs = 200);

    // 发起一次 RPC 调用
        //   method      ：方法名，须与 RpcServer::addHandler 注册的一致
        //   requestJson ：请求正文（JSON 字符串）
        //   responseJson：成功时填入响应正文
        //   返回 true = 调用成功
        bool call(const std::string &method, const std::string &requestJson,
                  std::string &responseJson);

  private:
  std::string ip_;
  uint16_t port_;
  int timeoutMs_;

  static uint32_t nextReqId();
};