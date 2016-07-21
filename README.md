CSci4061 S2016 Project 3

4/08/2016

Wendi An

4638209 

#server.c and shell.c

##1. Purpose
The purpose of this program is to use inturrupt-driven programming to create a program where a "sender" can send a message to a "receiver".

##2. Compile
To compile, use the Makefile included and simply type into the terminal:
```
make clean; make
```
##3. How to use from shell
This program must be given 6 arguments: user name (String), key (int), window size (int), maximum delay time (int), timeout limit (int), and drop rate (int, in percent).

To open the application from the shell, type:
```
./application <name> <key> <window_size> <max_delay> <timeout> <drop_rate>
```

##4. What the program does
There are two available roles for a user to pick in this program: sender and receiver. A sender user will be prompted by the program to enter a message and the name of the user to receive the message. Larger sized messages are broken down in smaller sized packets and randomly sent out of order to the receiver.  Every time a packet is sent to the receiver, the sender will be notified which packet was sent and the pid associated with the receiver's name. Every time a packet is delivered successfully, the receiver should retrieve the packet from their mailbox and send an acknowledgement message back to the sender. Once the sender has received an acknowledgment message for every packet sent, both users will be notified that all packets have been successfully sent and received for the message, and the message will be printed for the "receiver".

The effect of having a very short TIMEOUT is that the packet may delay for too long, causing the program to time out before the delays even finish.  This means often some successfully sent messages will be assumed to be failures.  A benefit of a short TIMEOUT is that the user does not have to wait too long for the program in case the message actually fails to send.

One possible negative effect of having a window size too large is that the message queue might run out of space for all the packets sent at the same time.  Besides that, there are no negative effects of declaring a large window size since the program automatically scales down the window if there are fewer packets than windows.  Having a smaller window simply means there will be fewer packets in transit at a time.


##5. Assumptions
We are assuming that there will be at least one "receiver" and one "sender" user running so that a message can be sent between two processes.  A sender must not send to another sender or itself, the message cannot contain space bars.  We changed the application.c file by adding a while loop when the user is asked to enter a role, this is to prevent signals from forcing the scanf to fail.  This does not change the behavior of the application, it is just an extra precaution taken so that the program does not continue until a legitimate role is entered by the user.  The changes were at lines 48-54:

```
scanf("%s", role);
```

was changed to:

```
printf("\nRole (sender/receiver): ");
int v = scanf("%s", role);
while (v == -1)
{
  v = scanf("%s", role);
}
```

##6. Error handling
System call failures will cause the program to exit.  All other errors will be user input errors when sending since receiving is "set-and-forget."  Improper sending technique will cause the program to simply fail to send messages and start over, prompting the user to choose a role.

________________
