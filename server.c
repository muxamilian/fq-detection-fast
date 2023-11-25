// Include the necessary libraries
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <limits.h>
#include <fcntl.h>
#include <string.h>
#include <sys/select.h>
#include <assert.h>

double get_unix_epoch_time() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

struct timeval timeout_zero;

// Define some constants
#define SEQ_LEN 4
#define TIMESTAMP_LEN 8
#define FROM_SOCKET_LEN 1

// Declare some global variables
int port;
int mtu;
int debug;
int socks[2];
int seq_nums[2];
struct sockaddr_in6 remote_addresses[2];
int payload_size;
int padding_sequence_len;
char *padding_sequence;
int recv_port;
int recv_socket;
double initial_rtt;
double latest_rtts[2];
int next_socket;
int port_indices[2];
char *data_buffer;
char *send_buffer;

// A function to parse the command line arguments
void parse_args(int argc, char *argv[]) {
  puts("Parsing arguments");
  // Initialize the default values
  port = 13579;
  mtu = 1500;
  debug = 0;
  // Loop through the arguments
  for (int i = 1; i < argc; i++) {
    // Check if the argument is a flag
    if (argv[i][0] == '-') {
      // Check which flag it is
      if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
        // Get the port number from the next argument
        i++;
        if (i < argc) {
          port = atoi(argv[i]);
        }
        else {
          // Invalid argument
          fprintf(stderr, "Missing port number\n");
          exit(1);
        }
      }
      else if (strcmp(argv[i], "--mtu") == 0) {
        // Get the mtu value from the next argument
        i++;
        if (i < argc) {
          mtu = atoi(argv[i]);
        }
        else {
          // Invalid argument
          fprintf(stderr, "Missing mtu value\n");
          exit(1);
        }
      }
      else if (strcmp(argv[i], "--debug") == 0) {
        // Set the debug flag to true
        debug = 1;
      }
      else {
        // Unknown flag
        fprintf(stderr, "Unknown flag: %s\n", argv[i]);
        exit(1);
      }
    }
    else {
      // Invalid argument
      fprintf(stderr, "Invalid argument: %s\n", argv[i]);
      exit(1);
    }
  }
}

// A function to create and bind the sockets
void create_sockets() {
  puts("create sockets");
  // Loop through the ports
  for (int i = 0; i < 2; i++) {
    // Create a socket
    socks[i] = socket(AF_INET6, SOCK_DGRAM, 0);
    if (socks[i] < 0) {
      // Socket creation failed
      perror("socket");
      exit(1);
    }
  // Disable IPV6_V6ONLY option
  int v6only = 0;
  if (setsockopt(socks[i], IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) == -1) {
      perror("Error setting IPV6_V6ONLY option");
      exit(EXIT_FAILURE);
  }
    // Create a sockaddr_in6 structure
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port + i);
    addr.sin6_addr = in6addr_any;
    // Bind the socket to the address
    if (bind(socks[i], (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      // Socket binding failed
      perror("bind");
      exit(1);
    }
    // Initialize the sequence number
    seq_nums[i] = 0;
  }
}

// A function to create and bind the receive socket
void create_recv_socket() {
  puts("create recv socket");
  // Create a socket
  recv_socket = socket(AF_INET6, SOCK_DGRAM, 0);
  if (recv_socket < 0) {
    // Socket creation failed
    perror("socket");
    exit(1);
  }
  // Disable IPV6_V6ONLY option
  int v6only = 0;
  if (setsockopt(recv_socket, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) == -1) {
      perror("Error setting IPV6_V6ONLY option");
      exit(EXIT_FAILURE);
  }
  // Create a sockaddr_in6 structure
  struct sockaddr_in6 addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(port + 2);
  addr.sin6_addr = in6addr_any;
  // Bind the socket to the address
  if (bind(recv_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    // Socket binding failed
    perror("bind");
    exit(1);
  }
  // Set the socket to non-blocking mode
  // if (fcntl(recv_socket, F_SETFL, O_NONBLOCK) < 0) {
  //   // Socket setting failed
  //   perror("fcntl");
  //   exit(1);
  // }
}

// A function to get an estimate of the RTT
void get_initial_rtt() {
  puts("Get initial rtt");
  // Create a sockaddr_in6 structure to store the client address
  struct sockaddr_in6 addr;
  socklen_t addrlen = sizeof(addr);
  // Receive a packet from the client
  char data[1500];
  puts("Listening");
  int n = recvfrom(recv_socket, data, 1500, 0, (struct sockaddr *)&addr, &addrlen);
  if (n < 0) {
    // Receive failed
    perror("recvfrom");
    exit(1);
  }
  puts("Received packet");
  // Store the client address
  remote_addresses[0] = addr;
  // Get the current time
  double t1 = get_unix_epoch_time();
  // Pack a message with the sequence number and the timestamp
  char ret_msg[SEQ_LEN + TIMESTAMP_LEN];
  unsigned int zero = 0;
  memcpy(ret_msg, &zero, sizeof(zero));
  memcpy(ret_msg + sizeof(zero), &t1, sizeof(t1));

  // Send the message to the client
  n = sendto(socks[0], ret_msg, sizeof(ret_msg), 0, (struct sockaddr *)&addr, addrlen);
  if (n < 0) {
    // Send failed
    perror("sendto");
    exit(1);
  }
  // Receive another packet from the client
  n = recvfrom(recv_socket, data, 1500, 0, (struct sockaddr *)&addr, &addrlen);
  if (n < 0) {
    // Receive failed
    perror("recvfrom");
    exit(1);
  }
  unsigned int received_zero;
  memcpy(&received_zero, data, sizeof(received_zero));
  double received_timestamp;
  memcpy(&received_timestamp, data + sizeof(received_zero), sizeof(received_timestamp));
  char sock_num;
  memcpy(&sock_num, data + sizeof(received_zero) + sizeof(received_timestamp), sizeof(sock_num));
  printf("Received packet with seq_num %d and timestamp %f and socknum %d\n", received_zero, received_timestamp, (int) sock_num);

  // Get the current time
  double t2 = get_unix_epoch_time();
  // Calculate the initial RTT
  initial_rtt = t2 - t1;
  // Print the debug information
  if (debug) {
    printf("Got connection from %s %d with an rtt of %.3f\n", inet_ntop(AF_INET6, &addr.sin6_addr, data, 1500), ntohs(addr.sin6_port), initial_rtt);
  }
}

// A function to detect fair queuing
void detect_fair_queuing() {
  puts("Trying to detect fair queuing");
  // Initialize the rates in packets per second
  double rates[2] = {15, 30};
  // Initialize the latest RTTs
  latest_rtts[0] = initial_rtt;
  latest_rtts[1] = initial_rtt;
  // Initialize the next socket to send from
  next_socket = 0;
  // Initialize the port indices
  port_indices[0] = 0;
  port_indices[1] = 1;
  // Allocate memory for the data buffer and the send buffer
  char data_buffer[SEQ_LEN + TIMESTAMP_LEN + FROM_SOCKET_LEN];
  send_buffer = malloc(payload_size);
  if (send_buffer == NULL) {
    // Memory allocation failed
    fprintf(stderr, "malloc error\n");
    exit(1);
  }
  // Copy the padding sequence to the send buffer
  memcpy(send_buffer + SEQ_LEN, padding_sequence, padding_sequence_len);
  // Run as many cycles as necessary to detect fair queuing
  for (int cycle_num = 0; cycle_num < 1/*INT_MAX*/; cycle_num++) {
    // Initialize the current sequence numbers at the beginning of the cycle
    int seq_nums_beginning[2];
    seq_nums_beginning[0] = seq_nums[0];
    seq_nums_beginning[1] = seq_nums[1];
    // Initialize the sequence numbers when enough packets were sent
    int seq_nums_end[2] = {-1, -1};
    // Initialize the number of packets acked in the current cycle
    int num_acked[2] = {0, 0};
    // Initialize the time the first ack was received
    double first_ack_times[2] = {-1, -1};
    // Initialize the time the second ack was received
    double last_ack_times[2] = {-1, -1};
    // Get the start time of the cycle
    double start_time = get_unix_epoch_time();
    // Initialize the time at which enough packets were sent for the measurement
    double send_end_time = -1;
    // Make sure to send at least one packet in each cycle
    double min_time = 1 / rates[0];
    // Get the time of the measurement. Maximum of the current RTTs of both subflows. At least 100ms.
    double time_to_run = fmax(fmax(fmax(latest_rtts[0], latest_rtts[1]), 0.1), min_time);
    double should_send[2];
    for (int should_send_i=0; should_send_i < 2; should_send_i++) {
      should_send[should_send_i] = rates[should_send_i] * time_to_run;
    }
    double rates_in_mbit[2];
    for (int rates_in_mbit_i=0; rates_in_mbit_i < 2; rates_in_mbit_i++) {
      rates_in_mbit[rates_in_mbit_i] = \
        rates[rates_in_mbit_i] * ((double) mtu)/1000000.0;
    }
    if (debug) {
      printf("Start cycle_num %d, rates %.1f %.1f, rates_in_mbit %.5f %.5f, time_to_run %.5f\n", cycle_num, rates[0], rates[1], rates_in_mbit[0], rates_in_mbit[1], time_to_run);
    }
    while (1) {
      double current_time = get_unix_epoch_time();
      // Check if enough packets were sent already
      if (seq_nums_end[0] == -1 && current_time > start_time + time_to_run) {
        seq_nums_end[0] = seq_nums[0]; seq_nums_end[1] = seq_nums[1];
        send_end_time = current_time;
      }
      while (1) {
        // Try to receive an acknowledgement from the client
        fd_set read_sds;

        FD_ZERO(&read_sds);
        FD_SET(recv_socket, &read_sds);

        int ret = select(recv_socket+1, &read_sds, NULL, NULL, &timeout_zero);
        if (ret < 0) {
            perror("Select error");
          exit(EXIT_FAILURE);
        } else if (ret == 0) {
          break;
        } else if (FD_ISSET(recv_socket, &read_sds)) {
          ssize_t bytes_received = recv(recv_socket, data_buffer, sizeof(data_buffer), 0);
          if (bytes_received == -1) {
            perror("Error receiving data");
          } else {
            unsigned int ack_num;
            double send_timestamp;
            char sock_index;
            memcpy(&ack_num, data_buffer, sizeof(ack_num));
            memcpy(&send_timestamp, data_buffer + sizeof(ack_num), sizeof(send_timestamp));
            memcpy(&sock_index, data_buffer + sizeof(ack_num) + sizeof(send_timestamp), sizeof(sock_index));
            printf("Server loop: Received packet with ack_num %d and timestamp %f and sock_index %d\n", ack_num, send_timestamp, (int) sock_index);
            assert(sock_index >=0 && sock_index <= 1);
            latest_rtts[sock_index] = current_time - send_timestamp;
            if (ack_num >= seq_nums_beginning[sock_index] && (seq_nums_end[0] == -1 || ack_num < seq_nums_end[sock_index])) {
              if (num_acked[sock_index] == 0) {
                // First ack received for this subflow
                first_ack_times[sock_index] = current_time;
              }
              // Add ack if it is relevant for the current measurement
              num_acked[sock_index] += 1;
            } else if (last_ack_times[sock_index] == -1 && seq_nums_end[0] != -1 && ack_num >= seq_nums_end[sock_index]) {
              // This was the last ack relevant for the current measurement
              last_ack_times[sock_index] = current_time;
            }
          }
        }
      }
    }
  }
}

int main(int argc, char *argv[]) {
  timeout_zero.tv_sec = 0;
  timeout_zero.tv_usec = 0;
  parse_args(argc, argv);
  printf("server: port: %d, mtu: %d, debug: %d\n", port, mtu, debug);
  // Subtract 40 for IPv6 and 8 for UDP
  payload_size = mtu - 40 - 8;
  padding_sequence_len = payload_size - SEQ_LEN - TIMESTAMP_LEN;
  padding_sequence = malloc(padding_sequence_len);
  for (int i = 0; i < padding_sequence_len; i++) {
    padding_sequence[i] = 'A';
  }
  create_sockets();
  create_recv_socket();
  get_initial_rtt();
  detect_fair_queuing();
  return 0;
}
