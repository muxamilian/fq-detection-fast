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
#include <assert.h>
#include <unistd.h>
#include <sys/ioctl.h>
#define likely(x)      __builtin_expect(!!(x), 1) 
#define unlikely(x)    __builtin_expect(!!(x), 0)

// Get the current time as a double representing the number of seconds since the epoch
double get_unix_epoch_time() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#define SEQ_LEN 4
#define TIMESTAMP_LEN 8
#define FROM_SOCKET_LEN 1

int port;
int mtu;
int debug;
int socks[2];
int seq_nums[2];
int payload_size;
int padding_sequence_len;
char *padding_sequence;
int recv_port;
int recv_socket;
double initial_rtt;
double latest_rtts[2];
int next_socket;
char *data_buffer;
char *send_buffer;

struct sockaddr_in6 remote_addr;
socklen_t remote_addrlen;

// Parse the command line arguments
void parse_args(int argc, char *argv[]) {
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
        fprintf(stderr, "Unknown flag: %s\n", argv[i]);
        exit(1);
      }
    }
    else {
      fprintf(stderr, "Invalid argument: %s\n", argv[i]);
      exit(1);
    }
  }
}

// Create and bind the sending sockets
void create_sockets() {
  // Loop through the ports
  for (int i = 0; i < 2; i++) {
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
    if (bind(socks[i], (struct sockaddr *) &addr, sizeof(addr)) < 0) {
      // Socket binding failed
      perror("bind");
      exit(1);
    }
    // Initialize the sequence number
    seq_nums[i] = 0;
  }
}

// Create and bind the receive socket
void create_recv_socket() {
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
}

// Get an estimate of the RTT
void get_initial_rtt() {
  // Create a sockaddr_in6 structure to store the client address
  remote_addrlen = sizeof(remote_addr);
  // Receive a packet from the client
  char data[1500];
  int n = recvfrom(recv_socket, data, 1500, 0, (struct sockaddr *)&remote_addr, &remote_addrlen);
  if (n < 0) {
    // Receive failed
    perror("recvfrom");
    exit(1);
  }
  for (int i=0; i<2; i++) {
    if (connect(socks[i], (struct sockaddr *) &remote_addr, remote_addrlen) != 0) {
      perror("Connect in server failed");
    }
  }
  // Get the current time
  double t1 = get_unix_epoch_time();
  // Pack a message with the sequence number and the timestamp
  char ret_msg[SEQ_LEN + TIMESTAMP_LEN];
  unsigned int zero = 0;
  memcpy(ret_msg, &zero, sizeof(zero));
  memcpy(ret_msg + sizeof(zero), &t1, sizeof(t1));

  // Send the message to the client
  n = send(socks[0], ret_msg, sizeof(ret_msg), 0);
  if (n < 0) {
    // Send failed
    perror("send");
    exit(1);
  }
  // Receive another packet from the client
  n = recv(recv_socket, data, 1500, 0);
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
    printf("Got connection from %s %d with an rtt of %.3f\n", inet_ntop(AF_INET6, &remote_addr.sin6_addr, data, 1500), ntohs(remote_addr.sin6_port), initial_rtt);
  }
}

// The main part: The function to detect fair queuing
void detect_fair_queuing() {
  int count;
  ssize_t bytes_received;
  unsigned int ack_num;
  double send_timestamp;
  char sock_index;
  double current_time;
  double next_send_time_delta;
  double packets_that_should_have_been_sent;
  double packets_that_were_not_sent_but_should_have;
  double delta_till_next_packet;
  double current_time_at_send;
  // Initialize the rates in packets per second
  double rates[2] = {15, 30};
  // Initialize the latest RTTs
  latest_rtts[0] = initial_rtt;
  latest_rtts[1] = initial_rtt;
  // Initialize the next socket to send from
  next_socket = 0;
  // Allocate memory for the data buffer and the send buffer
  char data_buffer[SEQ_LEN + TIMESTAMP_LEN + FROM_SOCKET_LEN];
  send_buffer = malloc(payload_size);
  if (send_buffer == NULL) {
    // Memory allocation failed
    fprintf(stderr, "malloc error\n");
    exit(1);
  }
  // Copy the padding sequence to the send buffer
  memcpy(send_buffer + SEQ_LEN + TIMESTAMP_LEN, padding_sequence, padding_sequence_len);
  // Run as many cycles as necessary to detect fair queuing
  for (int cycle_num = 0; cycle_num < INT_MAX; cycle_num++) {
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
    int should_send[2];
    for (int should_send_i=0; should_send_i < 2; should_send_i++) {
      should_send[should_send_i] = rates[should_send_i] * time_to_run;
    }
    double rates_in_mbit[2];
    for (int rates_in_mbit_i=0; rates_in_mbit_i < 2; rates_in_mbit_i++) {
      rates_in_mbit[rates_in_mbit_i] = \
        rates[rates_in_mbit_i] * 8 * ((double) mtu)/1000000.0;
    }
    if (debug) {
      printf("Start cycle_num %d, rates %.1f %.1f, rates_in_mbit %.5f %.5f, time_to_run %.5f\n", cycle_num, rates[0], rates[1], rates_in_mbit[0], rates_in_mbit[1], time_to_run);
    }
    while (1) {
      current_time = get_unix_epoch_time();
      // Check if enough packets were sent already
      if (seq_nums_end[0] == -1 && current_time > start_time + time_to_run) {
        seq_nums_end[0] = seq_nums[0]; seq_nums_end[1] = seq_nums[1];
        send_end_time = current_time;
      }
      while (1) {
        // Try to receive an acknowledgement from the client
        // ioctl checks whether there's data to read in the socket
        ioctl(recv_socket, FIONREAD, &count);
        if (unlikely(count == -1)) {
          perror("ioctl error");
          exit(EXIT_FAILURE);
        } else if (count == 0) {
          break;
        } else {
          bytes_received = recv(recv_socket, data_buffer, sizeof(data_buffer), 0);
          if (unlikely(bytes_received == -1)) {
            perror("Error receiving data");
          } else {
            memcpy(&ack_num, data_buffer, sizeof(ack_num));
            memcpy(&send_timestamp, data_buffer + sizeof(ack_num), sizeof(send_timestamp));
            memcpy(&sock_index, data_buffer + sizeof(ack_num) + sizeof(send_timestamp), sizeof(sock_index));
            // printf("Server loop: Received packet with ack_num %d and timestamp %f and sock_index %d\n", ack_num, send_timestamp, (int) sock_index);
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
      if (last_ack_times[0] != -1 && last_ack_times[1] != -1) {
        break;
      }
      next_send_time_delta = INFINITY;
      for (int i=0; i<2; i++) {
        packets_that_should_have_been_sent = floor(rates[i]*(current_time-start_time));
        packets_that_were_not_sent_but_should_have = packets_that_should_have_been_sent - (seq_nums[i] - seq_nums_beginning[i]);
        if (packets_that_were_not_sent_but_should_have <= 0) {
            delta_till_next_packet = start_time + (packets_that_should_have_been_sent + 1)/rates[i] - current_time;
        } else {
          delta_till_next_packet = -packets_that_were_not_sent_but_should_have;
        }
        if (delta_till_next_packet <= next_send_time_delta) {
          next_socket = i;
          next_send_time_delta = delta_till_next_packet;
        }
      }
      if (next_send_time_delta > 0) {
        // Convert to microseconds
        usleep((unsigned int) (next_send_time_delta * 1000000));
      }
      memcpy(send_buffer, &(seq_nums[next_socket]), sizeof(seq_nums[next_socket]));
      current_time_at_send = current_time + fmax(next_send_time_delta, 0);
      memcpy(send_buffer + sizeof(seq_nums[next_socket]), &current_time_at_send, sizeof(current_time_at_send));
      ssize_t n = send(socks[next_socket], send_buffer, payload_size, 0);
      if (n < 0) {
        // Send failed
        perror("send");
        exit(1);
      }
      seq_nums[next_socket] += 1;
    }
    int packets_actually_sent[2];
    for (int packets_actually_sent_i=0; packets_actually_sent_i<2; packets_actually_sent_i++) {
      packets_actually_sent[packets_actually_sent_i] = seq_nums_end[packets_actually_sent_i] - seq_nums_beginning[packets_actually_sent_i];
    }
    // Could the link be saturated?
    int sent_enough[2];
    for (int sent_enough_i=0; sent_enough_i<2; sent_enough_i++) {
      sent_enough[sent_enough_i] = ceil(packets_actually_sent[sent_enough_i]) + 1 >= should_send[sent_enough_i] * 15.0/16.0;
    }
    int rtts_ms[2];
    for (int latest_rtts_i=0; latest_rtts_i<2; latest_rtts_i++) {
      rtts_ms[latest_rtts_i] = round(latest_rtts[latest_rtts_i]*1000);
    }
    double receiving_rate1 = (num_acked[0]/(last_ack_times[0]-first_ack_times[0]));
    double receiving_rate2 = (num_acked[1]/(last_ack_times[1]-first_ack_times[1]));
    double sending_rate1 = (packets_actually_sent[0]/(send_end_time-start_time));
    double sending_rate2 = (packets_actually_sent[1]/(send_end_time-start_time));
    // Ratio of receiving rate over sending rate for the first flow
    double first_ratio = receiving_rate1/sending_rate1;
    // Ratio of receiving rate over sending rate for the second flow
    double second_ratio = receiving_rate2/sending_rate2;
    if (debug) {
      printf("End cycle_num:%d,packets_actually_sent:%d,%d,rtts_ms:%d,%d,sent_enough:%d,%d,seq_nums_beginning:%d,%d,should_send:%d,%d,seq_nums_end:%d,%d,num_acked:%d,%d,seq_nums:%d,%d,first_ratio:%.3f,first_ratio:%.3f\n", cycle_num, packets_actually_sent[0], packets_actually_sent[1], rtts_ms[0], rtts_ms[1], sent_enough[0], sent_enough[1], seq_nums_beginning[0], seq_nums_beginning[1], should_send[0], should_send[1], seq_nums_end[0], seq_nums_end[1], num_acked[0], num_acked[1], seq_nums[0], seq_nums[1], first_ratio, second_ratio); 
    }
    if (second_ratio < 0.5) {
      double loss_ratio = first_ratio/second_ratio;
      if (loss_ratio >= 1.5) {
        // This means that second flow sent a lot more but couldn't get more data to the client
        double confidence = fmin((loss_ratio-1.5)*2, 1);
        int rounded_confidence = round(confidence*100);
        printf("Fair queuing detected with a confidence of %d%%\n", rounded_confidence);
      } else {
        // The second flow sent more and got more data to the client
        double confidence = fmin(1-((loss_ratio-1)*2), 1);
        int rounded_confidence = round(confidence*100);
        printf("First-come first-served detected with a confidence of %d%%\n", rounded_confidence);
      }
      break;
    } else if (sent_enough[0] == 0 || sent_enough[1] == 0) {
      double managed_to_send_mbit = (sending_rate1+sending_rate2)*8*mtu/1000000;
      double wanted_to_send = rates_in_mbit[0] + rates_in_mbit[1]; 
      printf("Failed to utilize the link. Tried to send %.3f Mbit/s but only managed %.3f Mbit/s. Aborting\n", wanted_to_send, managed_to_send_mbit);
      break;
    }
    // If nothing happened, double the sending rate, to try to saturate the link
    rates[0] *= 2;
    rates[1] *= 2;    
  }
}

int main(int argc, char *argv[]) {
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
