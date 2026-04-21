#include "Channel.h"
#include "Epoll.h"
#include "InetAddress.h"
#include "Socket.h"
#include "util.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include <vector>

#define READ_BUFFER 1024

int main() {
  Socket *serv_sock = new Socket();
  InetAddress *serv_addr = new InetAddress("127.0.0.1", 8888);
  serv_sock->bind(serv_addr);
  serv_sock->listen();
  Epoll *ep = new Epoll();
  serv_sock->setnonblocking();

  Channel *servChannel = new Channel(ep, serv_sock->getFd());
  servChannel->enableReading();
  std::cout << "[server] Server start success!" << std::endl;

  while (true) {
    std::vector<Channel *> activeChannels = ep->poll();
    int nfds = activeChannels.size();
    for (int i = 0; i < nfds; ++i) {
      int chFd = activeChannels[i]->getFd();

      if (chFd == serv_sock->getFd()) {
        // 新连接
        InetAddress *client_addr = new InetAddress();
        int client_sockfd = serv_sock->accept(client_addr);

        std::cout << "[server] new client fd " << client_sockfd
                  << "! IP:" << inet_ntoa(client_addr->addr.sin_addr)
                  << " Port: " << ntohs(client_addr->addr.sin_port)
                  << std::endl;

        Socket *client_sock = new Socket(client_sockfd);
        client_sock->setnonblocking();

        Channel *clientChannel = new Channel(ep, client_sockfd);
        clientChannel->enableReading();
        delete client_addr;
      } else if (activeChannels[i]->getRevents() & POLLER_READ) {
        // 数据可读
        char buf[READ_BUFFER];
        while (true) {
          bzero(buf, sizeof(buf));
          ssize_t bytes_read = read(chFd, buf, sizeof(buf));
          if (bytes_read > 0) {
            std::cout << "[server] message received from client fd " << chFd
                      << ": " << buf << std::endl;
            write(chFd, buf, bytes_read);
          } else if (bytes_read == -1 && errno == EINTR) {
            continue;
          } else if (bytes_read == -1 &&
                     ((errno == EAGAIN) || (errno == EWOULDBLOCK))) {
            break;
          } else if (bytes_read == 0) {
            std::cout << "[server] EOF received from client fd " << chFd
                      << " and it's now disconnected" << std::endl;
            close(chFd);
            delete activeChannels[i];
            activeChannels[i] = nullptr;
            break;
          }
        }
      }
    }
  }
  delete serv_sock;
  delete serv_addr;
  delete ep;
  return 0;
}
