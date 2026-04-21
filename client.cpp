#include "include/util.h"
#include <arpa/inet.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <ostream>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int main() {
  // 初始化本机socket
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  errif(sockfd == -1, "[客户端] socket创建失败");

  // 初始化服务器地址
  struct sockaddr_in server_addr;
  std::memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  server_addr.sin_port = htons(8888);
  // 对应server那边的地址

  errif(connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) ==
            -1,
        "[客户端] 与server建立连接时失败\n");

  std::cout << "[客户端] 成功与server建立TCP连接\n";

  while (true) {
    char buf[1024];
    bzero(buf, sizeof(buf));
    if (fgets(buf, sizeof(buf), stdin) == NULL) {
      // 读到EOF退出客户端
      break;
    }
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
      buf[len - 1] = '\0';
      len--;
    }
    if (len == 0)
      continue;

    ssize_t write_bytes = write(sockfd, buf, sizeof(buf));

    if (write_bytes == -1) {
      std::cout << "[客户端] socket连接已经关闭，无法再写入数据\n";
      break;
    }

    bzero(buf, sizeof(buf));
    ssize_t read_bytes = read(sockfd, buf, sizeof(buf));
    if (read_bytes > 0) {
      std::cout << "[客户端] 收到来自服务器的数据：\n" << buf << std::endl;
    } else if (read_bytes == 0) {
      std::cout << "[客户端] 服务器 socket 断开连接\n";
      break;
    } else if (read_bytes == -1) {
      close(sockfd);
      errif(true, "[客户端] socket 读取失败");
    }
  }

  close(sockfd);
  return 0;
}