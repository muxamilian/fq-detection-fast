// Include the necessary libraries
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <assert.h>

// Define some constants
#define DEFAULT_PORT 13579
#define DEFAULT_SERVER_ADDRESS "127.0.0.1"
#define SEQ_NUM_LEN 4
#define TIMESTAMP_LEN 8
#define TIMEOUT 5

// The client basically only echoes acknowledgements to the server
// The server code is more interesting
int main(int argc, char *argv[])
{
    // Parse the command line arguments
    int port = DEFAULT_PORT;
    char *server_address = DEFAULT_SERVER_ADDRESS;
    int ipv6 = 0;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0)
        {
            port = atoi(argv[i + 1]);
            i++;
        }
        else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--server-address") == 0)
        {
            server_address = argv[i + 1];
            i++;
        }
        else if (strcmp(argv[i], "--ipv6") == 0)
        {
            ipv6 = 1;
        }
        else
        {
            fprintf(stderr, "Invalid argument: %s\n", argv[i]);
            exit(1);
        }
    }
    printf("port: %d, server_address: %s, ipv6: %d\n", port, server_address, ipv6);
    int ports[2] = {port, port + 1};

    // Create a socket
    int address_family = ipv6 ? AF_INET6 : AF_INET;
    int sock = socket(address_family, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        perror("socket");
        exit(1);
    }

    // Get IP address from host name
    struct addrinfo hints, *resolved_server_address;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = address_family;
    hints.ai_socktype = SOCK_DGRAM;
    char port_str[6];
    printf("Sending to port %d\n", port+2);
    sprintf(port_str, "%d", port+2);
    int err = getaddrinfo(server_address, port_str, &hints, &resolved_server_address);
    if (err != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        exit(1);
    }

    // Address of the server and port number
    struct sockaddr *base_addr = resolved_server_address->ai_addr;
    socklen_t base_addr_len = resolved_server_address->ai_addrlen;

    // Send some kind of handshake
    uint32_t seq_num = 0;
    sendto(sock, &seq_num, sizeof(seq_num), 0, base_addr, base_addr_len);

    // Just echo everything back forever
    while (1)
    {
        // Receive packet from server or timeout
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        struct timeval tv;
        tv.tv_sec = TIMEOUT;
        tv.tv_usec = 0;
        int n = select(sock + 1, &readfds, NULL, NULL, &tv);
        if (n == 0)
        {
            printf("Timeout in client\n");
            break;
        }
        else if (n < 0)
        {
            perror("select");
            exit(1);
        }
        char data[SEQ_NUM_LEN + TIMESTAMP_LEN + 1];
        struct sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);
        recvfrom(sock, data, sizeof(data), 0, (struct sockaddr *)&addr, &addr_len);
        int sock_num = -1;
        for (int i = 0; i < 2; i++)
        {
            if (ports[i] == ntohs(((struct sockaddr_in *)&addr)->sin_port))
            {
                sock_num = i;
                break;
            }
        }
        // printf("sock_num is %d\n", sock_num);
        // Echo back and tell the server from port it came, the lower one or the higher one.
        // This is encoded in `sock_num`
        // assert(sock_num >= -1 && sock_num <= 1);
        data[SEQ_NUM_LEN + TIMESTAMP_LEN + 1 - 1] = (char) sock_num;
        sendto(sock, data, sizeof(data), 0, base_addr, base_addr_len);
    }

    // Close the socket and free the memory
    close(sock);
    freeaddrinfo(resolved_server_address);
    return 0;
}
