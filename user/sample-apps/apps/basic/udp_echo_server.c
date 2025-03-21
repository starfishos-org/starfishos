#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT    9001
#define BUFSIZE 4096

int main(int argc, char *argv[])
{
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));

        struct sockaddr_in serveraddr;
        struct sockaddr_in clientaddr;

        bzero(&serveraddr, sizeof(serveraddr));
        serveraddr.sin_family = AF_INET;
        serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
        serveraddr.sin_port = htons(PORT);

        bind(sock, (struct sockaddr *)&serveraddr, sizeof(serveraddr));
        printf("UDP Server is running at 0.0.0.0:%d\n", PORT);

        char buf[BUFSIZE];
        while (1) {
                bzero(buf, BUFSIZE);

                socklen_t clientaddrlen = sizeof(clientaddr);
                ssize_t n = recvfrom(sock,
                                     buf,
                                     BUFSIZE,
                                     0,
                                     (struct sockaddr *)&clientaddr,
                                     &clientaddrlen);
                if (n < 0) {
                        printf("Failed to receive\n");
                        break;
                }

                char clientip[16] = {0};
                inet_ntop(AF_INET,
                          &clientaddr.sin_addr,
                          clientip,
                          sizeof(clientip));

                printf("Received from %s:%hu, n: %zd, data: %s\n",
                       clientip,
                       ntohs(clientaddr.sin_port),
                       n,
                       buf);

                sendto(sock,
                       buf,
                       n,
                       0,
                       (struct sockaddr *)&clientaddr,
                       clientaddrlen);
        }

        return 0;
}
