#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#include "process.h"

// messaging config
int WINDOW_SIZE;
int MAX_DELAY;
int TIMEOUT;
int DROP_RATE;
//for sig handlers
struct sigaction alm;
struct sigaction rec;
// process information
process_t myinfo;
int mailbox_id;
// a message id is used by the receiver to distinguish a message from other messages
// you can simply increment the message id once the message is completed
int message_id = 0;

// the message status is used by the sender to monitor the status of a message
message_status_t message_stats;

// the message is used by the receiver to store the actual content of a message
message_t *message;

int num_available_packets; // number of packets that can be sent (0 <= n <= WINDOW_SIZE)
int total_packets_needed; //this is how the receiver knows when the message is done
int is_receiving = 0; // a helper varibale may be used to handle multiple senders'
int ntimeouts;  // keeps track of how many timeouts

/**
 * complete the definition of the function
 * 1. Save the process information to a file and a process structure for future use.
 * 2. Setup a message queue with a given key.
 * 3. Setup the signal handlers (SIGIO for handling packet, SIGALRM for timeout).
 * Return 0 if success, -1 otherwise.
 */
int init(char *process_name, key_t key, int wsize, int delay, int to, int drop) {
    myinfo.pid = getpid();
    strcpy(myinfo.process_name, process_name);
    myinfo.key = key;

    // open the file
    FILE* fp = fopen(myinfo.process_name, "wb");
    if (fp == NULL) {
        printf("Failed opening file: %s\n", myinfo.process_name);
        return -1;
    }
    // write the process_name and its message keys to the file
    if (fprintf(fp, "pid:%d\nprocess_name:%s\nkey:%d\n", myinfo.pid, myinfo.process_name, myinfo.key) < 0) {
        printf("Failed writing to the file\n");
        return -1;
    }
    fclose(fp);

    WINDOW_SIZE = wsize;
    MAX_DELAY = delay;
    TIMEOUT = to;
    DROP_RATE = drop;

    printf("[%s] pid: %d, key: %d\n", myinfo.process_name, myinfo.pid, myinfo.key);
    printf("window_size: %d, max delay: %d, timeout: %d, drop rate: %d%%\n", WINDOW_SIZE, MAX_DELAY, TIMEOUT, DROP_RATE);

    // setup a message queue and save the id to the mailbox_id
    mailbox_id = msgget(myinfo.key, IPC_CREAT | 0666);
    if(mailbox_id == -1)
    {
        perror("msgget failed");
        return -1;
    }

    //setting up signal handlers
    rec.sa_handler = receive_packet;
    rec.sa_flags = 0;
    if (sigaction(SIGIO,&rec,NULL) != 0)
    {
      perror("sig io handler failed to initialize");
      return -1;
    }
    signal(SIGIO, SIG_IGN);
    alm.sa_handler = timeout_handler;
    alm.sa_flags = 0;
    if (sigaction(SIGALRM,&alm,NULL) != 0)
    {
      perror("sig alarm handler failed to initialize");
      return -1;
    }
    return 0;
}

/**
 * Get a process' information and save it to the process_t struct.
 * Return 0 if success, -1 otherwise.
 */
int get_process_info(char *process_name, process_t *info) {
    char buffer[MAX_SIZE];
    char *token;

    // open the file for reading
    FILE* fp = fopen(process_name, "r");
    if (fp == NULL) {
        return -1;
    }
    // parse the information and save it to a process_info struct
    while (fgets(buffer, MAX_SIZE, fp) != NULL) {
        token = strtok(buffer, ":");
        if (strcmp(token, "pid") == 0) {
            token = strtok(NULL, ":");
            info->pid = atoi(token);
        } else if (strcmp(token, "process_name") == 0) {
            token = strtok(NULL, ":");
            strcpy(info->process_name, token);
        } else if (strcmp(token, "key") == 0) {
            token = strtok(NULL, ":");
            info->key = atoi(token);
        }
    }
    fclose(fp);
    return 0;
}

/**
 * Send a packet to a mailbox identified by the mailbox_id, and send a SIGIO to the pid.
 * Return 0 if success, -1 otherwise.
 */
int send_packet(packet_t *packet, int mailbox_id, int pid)
{
  //sending packet to mailbox
  int nwrite = msgsnd(mailbox_id, packet, sizeof(packet_t), 0);
  if (nwrite < 0)
  {
    perror("failed to send packet");
    return -1;
  }
  printf("Send a packet [%d] to pid:%d\n", packet->packet_num, pid);
  //generating signal to inform pid that there is a packet in their queue
  if (kill(pid, SIGIO) < 0)
  {
    perror("failed to send signal packet");
    return -1;
  }
  return 0;
}

/**
 * Get the number of packets needed to send a data, given a packet size.
 * Return the number of packets if success, -1 otherwise.
 */
int get_num_packets(char *data, int packet_size) {
    if (data == NULL) {
        return -1;
    }
    if (strlen(data) % packet_size == 0) {
        return strlen(data) / packet_size;
    } else {
        return (strlen(data) / packet_size) + 1;
    }
}

/**
 * Create packets for the corresponding data and save it to the message_stats.
 * Return 0 if success, -1 otherwise.
 */
int create_packets(char *data, message_status_t *message_stats) {
    if (data == NULL || message_stats == NULL) {
        return -1;
    }
    int i, len;
    for (i = 0; i < message_stats->num_packets; i++) {
        if (i == message_stats->num_packets - 1) {
            len = strlen(data)-(i*PACKET_SIZE);
        } else {
            len = PACKET_SIZE;
        }
        message_stats->packet_status[i].is_sent = 0;
        message_stats->packet_status[i].ACK_received = 0;
        message_stats->packet_status[i].packet.message_id = -1;
        message_stats->packet_status[i].packet.mtype = DATA;
        message_stats->packet_status[i].packet.pid = myinfo.pid;
        strcpy(message_stats->packet_status[i].packet.process_name, myinfo.process_name);
        message_stats->packet_status[i].packet.num_packets = message_stats->num_packets;
        message_stats->packet_status[i].packet.packet_num = i;
        message_stats->packet_status[i].packet.total_size = strlen(data);
        memcpy(message_stats->packet_status[i].packet.data, data+(i*PACKET_SIZE), len);
        message_stats->packet_status[i].packet.data[len] = '\0';
    }
    return 0;
}

/**
 * Get the index of the next packet to be sent.
 * Return the index of the packet if success, -1 otherwise.
 */
int get_next_packet(int num_packets) {
    int packet_idx = rand() % num_packets;
    int i = 0;

    i = 0;
    while (i < num_packets) {
        if (message_stats.packet_status[packet_idx].is_sent == 0) {
            // found a packet that has not been sent
            return packet_idx;
        } else if (packet_idx == num_packets-1) {
            packet_idx = 0;
        } else {
            packet_idx++;
        }
        i++;
    }
    // all packets have been sent
    return -1;
}

/**
 * Use probability to simulate packet loss.
 * Return 1 if the packet should be dropped, 0 otherwise.
 */
int drop_packet() {
    if (rand() % 100 > DROP_RATE) {
        return 0;
    }
    return 1;
}

/* Send "pack" number of packets that have a status of sent but no ACK received yet to receiver
*/
void re_send(int packs)
{
  //printf("resend called, resending %d\n", packs);
  int i, next_packet;
  //checking the status of all packets to determine which packets should be sent again
  for (i = 0; i < packs; i++)
  {
    if ((message_stats.packet_status[i].ACK_received == 0) && (message_stats.packet_status[i].is_sent == 1))
    {
      //sending necessary packets again
      if (send_packet(&message_stats.packet_status[i].packet, message_stats.mailbox_id, message_stats.receiver_info.pid) < 0)
      {
        perror("send_packet failed");
        exit(-1);
      }
    }
  }
}

/*Send "packs" number of packets that has never been sent to the receiver. The get_next_packet function is used to
determine which number packet to send to the receiver.
*/
void send_new(int packs)
{
  //printf("sendnew called\n");
  int i, next_packet;
  for (i = 0; i < packs; i++)
  {
    next_packet = get_next_packet(message_stats.num_packets);
    if (next_packet == -1) break;
    message_stats.packet_status[next_packet].is_sent = 1;
    //Sending packet to receiver
    if (send_packet(&message_stats.packet_status[next_packet].packet, message_stats.mailbox_id, message_stats.receiver_info.pid) < 0)
    {
      perror("send_packet failed");
      exit(-1);
    }
  }
}


/**
 * Send a message (broken down into multiple packets) to another process.
 * We first need to get the receiver's information and construct the status of
 * each of the packet.
 * Return 0 if success, -1 otherwise.
 */
int send_message(char *receiver, char* content) {
    if (receiver == NULL || content == NULL) {
        printf("Receiver or content is NULL\n");
        return -1;
    }
    // get the receiver's information
    if (get_process_info(receiver, &message_stats.receiver_info) < 0) {
        printf("Failed getting %s's information.\n", receiver);
        return -1;
    }
    // get the receiver's mailbox id
    message_stats.mailbox_id = msgget(message_stats.receiver_info.key, 0666);
    if (message_stats.mailbox_id == -1) {
        printf("Failed getting the receiver's mailbox.\n");
        return -1;
    }
    // get the number of packets
    int num_packets = get_num_packets(content, PACKET_SIZE);
    if (num_packets < 0) {
        printf("Failed getting the number of packets.\n");
        return -1;
    }
    // set the number of available packets
    if (num_packets > WINDOW_SIZE) {
        num_available_packets = WINDOW_SIZE;
    } else {
        num_available_packets = num_packets;
    }
    // setup the information of the message
    message_stats.is_sending = 1;
    message_stats.num_packets_received = 0;
    message_stats.num_packets = num_packets;
    message_stats.packet_status = malloc(num_packets * sizeof(packet_status_t));
    if (message_stats.packet_status == NULL) {
        return -1;
    }
    // parition the message into packets
    if (create_packets(content, &message_stats) < 0) {
        printf("Failed paritioning data into packets.\n");
        message_stats.is_sending = 0;
        free(message_stats.packet_status);
        return -1;
    }
    // send packets to the receiver
    // the number of packets sent at a time depends on the WINDOW_SIZE.
    // you need to change the message_id of each packet (initialized to -1)
    // with the message_id included in the ACK packet sent by the receiver
    if (sigaction(SIGIO,&rec,NULL) != 0)
    {
      perror("sig io handler failed to initialize");
      return -1;
    }
    while (get_packet_from_mailbox(mailbox_id) != 0)
    {
      packet_t buf;
      msgrcv(mailbox_id, (void *) &buf, sizeof(packet_t), 0, 0);
    }
    int next_packet, i;
    ntimeouts = 0;
    //sending "num_available_packets" to reciever
    send_new(1);
    //Setting up the timeout alarm
    alarm(TIMEOUT);
    while (message_stats.is_sending == 1) pause();
    //rec.sa_handler = SIG_DFL;
    signal(SIGIO, SIG_IGN);
    if (ntimeouts == MAX_TIMEOUT)
    {
      printf("Timed out too many times.\n");
      free(message_stats.packet_status);
      return -1;
    }
    free(message_stats.packet_status);
    return 0;
}

/**
 * Handle TIMEOUT. Resend previously sent packets whose ACKs have not been
 * received yet. Reset the TIMEOUT.
 */
void timeout_handler(int sig)
{
  //Blocking signals
  sigset_t signal_set;
  sigfillset(&signal_set);
  sigprocmask(SIG_BLOCK, &signal_set, NULL);
  ntimeouts++;
  //Checking if if TIMEOUT_MAX has been reached
  if (ntimeouts == MAX_TIMEOUT)
  {
    printf("timeouts :: %d\n", ntimeouts);
    message_stats.is_sending = 0;
    sigprocmask(SIG_UNBLOCK, &signal_set, NULL);
    return;
  }
  printf("TIMEOUT!\n");
  re_send(message_stats.num_packets);
  //Unblocking
  sigprocmask(SIG_UNBLOCK, &signal_set, NULL);
  alarm(TIMEOUT);
}

/**
 * Send an ACK to the sender's mailbox.
 * The message id is determined by the receiver and has to be included in the ACK packet.
 * Return 0 if success, -1 otherwise.
 */
int send_ACK(int mailbox_id, pid_t pid, int packet_num) {
    // constructing an ACK packet
    packet_t *ack;
    ack = (packet_t *) malloc(sizeof(packet_t));
    ack->mtype = ACK;
    ack->message_id = message_id;
    ack->pid = pid;
    ack->packet_num = packet_num;
    int delay = rand() % MAX_DELAY;
    sleep(delay);
    // sending the ACK packet
    int nwr = msgsnd(mailbox_id, ack, sizeof(packet_t), 0);
    if(nwr < 0)
    {
        perror("failed to send ACK");
        return -1;
    }
    //sending SIGIO signal to pid
    if (kill(pid, SIGIO) < 0)
    {
      perror("failed to send signal packet for ACK");
      return -1;
    }
    free(ack);
    return 0;
}

/**
 * Handle DATA packet. Save the packet's data and send an ACK to the sender.
 * You should handle unexpected cases such as duplicate packet, packet for a different message,
 * packet from a different sender, etc.
 */
void handle_data(packet_t *packet, process_t *sender, int sender_mailbox_id)
{
  if (is_receiving == 0) return;
  // tests if this is the current message with current sender or a brand new message
  int should_enter = (((packet->message_id == message_id) && (sender->pid == message->sender.pid)) ||
        ((packet->message_id == -1) && (message->sender.pid == 0)));
  if (should_enter == 1)
  {
    if (packet->message_id == -1)
    {
      //if it is the first packet of the message
      total_packets_needed = packet->num_packets;
      if (get_process_info(packet->process_name, &message->sender) == -1)
      {
        printf("Failed getting %s's information.\n", packet->process_name);
        exit(-1);
      }
      sender_mailbox_id = msgget(message->sender.key, 0666);
    }
    //Constructing message from data packets
    int i = 0;
    int offset = packet->packet_num * PACKET_SIZE;
    if (message->data[offset + i] == '\0') // if it is not a duplicate:
    {
      for (i = 0; i < PACKET_SIZE; i++)
      {
        message->data[offset + i] = packet->data[i];
      }
      message->num_packets_received++;
    }
    //Must sending ACK if data packet was received
    if(send_ACK(sender_mailbox_id, sender->pid, packet->packet_num) < 0)
    {
      perror("send_ACK failed");
      exit(-1);
    }
    printf("Send an ACK for packet %d to pid:%d\n", packet->packet_num, sender->pid);
    if (total_packets_needed == message->num_packets_received)
    {
      message->is_complete = 1;
      return;
    }
  }
}

void set_mess_id(int id)
{
  int i;
  for (i = 0; i < message_stats.num_packets; i++)
  {
    message_stats.packet_status[i].packet.message_id = id;
  }
}

/**
 * Handle ACK packet. Update the status of the packet to indicate that the packet
 * has been successfully received and reset the TIMEOUT.
 * You should handle unexpected cases such as duplicate ACKs, ACK for completed message, etc.
 */
void handle_ACK(packet_t *packet)
{
  if (message_stats.is_sending == 0)
  {
    return;
  }
  int i = packet->packet_num;
  //Receiving an ACK
  if (message_stats.packet_status[i].ACK_received == 0)
  {
    printf("Received an ACK for packet [%d]\n", i);
    message_stats.packet_status[i].ACK_received = 1;
    message_stats.num_packets_received++;
    //If an ACK has been received, we send another if there are still more packets to be sent
    if (message_stats.num_packets_received != message_stats.num_packets)
    {
        send_new(1);
    }
  }
  if (message_stats.packet_status[0].packet.message_id == -1)
  {
    set_mess_id(packet->message_id);
    send_new(num_available_packets - 1);
  }
  //Received all ACKS for packets
  if (message_stats.num_packets_received == message_stats.num_packets)
  {
    alarm(0);
    message_stats.is_sending = 0;
    printf("All packets sent.\n");
    return;
  }
}

/*
 * Get the next packet (if any) from a mailbox.
 * Return 0 (false) if there is no packet in the mailbox
 */
int get_packet_from_mailbox(int mailbox_id) {
    struct msqid_ds buf;
    return (msgctl(mailbox_id, IPC_STAT, &buf) == 0) && (buf.msg_qnum > 0);
}

/**
 * Receive a packet.
 * If the packet is DATA, send an ACK packet and SIGIO to the sender.
 * If the packet is ACK, update the status of the packet.
 */
void receive_packet(int sig)
{
  //Blocking Signals
  sigset_t signal_set;
  sigfillset(&signal_set);
  sigprocmask(SIG_BLOCK, &signal_set, NULL);
  if (drop_packet())
  {
    sigprocmask(SIG_UNBLOCK, &signal_set, NULL);
    return;
  }
  else
  {
    packet_t buf;
    while (get_packet_from_mailbox(mailbox_id) != 0)
    {
      //retrieving packet from message queue
      int nread = msgrcv(mailbox_id, (void *) &buf, sizeof(packet_t), 0, 0);
      if (nread < 0)
      {
        perror("Failed to receive message, this shouldn't even be happening\n");
        sigprocmask(SIG_UNBLOCK, &signal_set, NULL);
        exit(-1);
      }
      //handling a data packet
      if (buf.mtype == DATA)
      {
        int id = msgget(message->sender.key, 0666);
        handle_data(&buf, &message->sender, id);
      }
      //handeling an ACK packet
      else
      {
        handle_ACK(&buf);
      }
    }
    // Unblocking
    sigprocmask(SIG_UNBLOCK, &signal_set, NULL);
  }
}

/**
 * Initialize the message structure and wait for a message from another process.
 * Save the message content to the data and return 0 if success, -1 otherwise
 */
int receive_message(char *data)
{
  if (sigaction(SIGIO,&rec,NULL) != 0)
  {
    perror("sig io handler failed to initialize");
    return -1;
  }
  while (get_packet_from_mailbox(mailbox_id) != 0)
  {
    packet_t buf;
    msgrcv(mailbox_id, (void *) &buf, sizeof(packet_t), 0, 0);
  }
  //printf("called receive message\n");
  message = (message_t *) malloc(sizeof(message_t));
  message->num_packets_received = 0;
  message->is_complete = 0;
  message->is_received = 0;
  message->data = (char *) malloc(MAX_SIZE * sizeof(char));
  int i;
  for (i = 0; i < MAX_SIZE; i++)
  {
    message->data[i] = '\0';
  }
  message->sender.pid = 0;
  message->sender.process_name[0] = '\0';
  message->sender.key = 0;
  is_receiving = 1;
  //Stays in loop until message is complete
  while (message->is_complete == 0) pause();
  signal(SIGIO, SIG_IGN);
  strcpy(data, message->data);
  //deallocated memory
  free(message->data);
  free(message);
  message_id++;
  is_receiving = 0;
  printf("All packets received.\n");
  return 0;
}
