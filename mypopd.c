#include "netbuffer.h"
#include "mailuser.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

#define MAX_LINE_LENGTH 1024

static void handle_client(int fd);
//global mail list for user in current child process
mail_list_t userMail;

int main(int argc, char *argv[]) {
  
    if (argc != 2) {
	fprintf(stderr, "Invalid arguments. Expected: %s <port>\n", argv[0]);
	return 1;
    }
  
    run_server(argv[1], handle_client);
  
    return 0;
}

void handle_client(int fd) {
  
    char recvbuf[MAX_LINE_LENGTH + 1];
    char *pointers[MAX_LINE_LENGTH + 1];
    net_buffer_t nb = nb_create(fd, MAX_LINE_LENGTH);
  
    //tracking the current stage
    int authorization = 1;
    int transaction = 0;
    char * user = NULL;

    //welcome message
    send_formatted(fd, "+OK POP3 server ready\r\n");
    //begin polling for client input
    while(1) {
        int ret = nb_read_line(nb, recvbuf);
        //check if client exited without QUIT
        if (ret == 0) {
            send_formatted(fd, "-ERR\r\n");
            break;
        }
        if (ret < 0) {
            send_formatted(fd, "-ERR\r\n");
            break;
        }
        //whitespace input and too-short inputs
        if (ret < 3) {
            send_formatted(fd, "-ERR\r\n");
            continue;
        }
        //split the args and store the number of args for guard clauses
        int numOfParts = split(recvbuf, pointers);
        if (numOfParts == 1 && strcasecmp("QUIT", pointers[0]) == 0) {
            if (transaction) {
                send_formatted(fd, "+OK (%d messages left)\r\n", get_mail_count(userMail, 0));
                destroy_mail_list(userMail);
            } else {
                send_formatted(fd, "+OK\r\n");
            }
            if (user != NULL) {
                    free(user);
                    user = NULL;
            }
            break;
        } else if (numOfParts == 1 && strcasecmp("NOOP", pointers[0]) == 0 && transaction) {
            send_formatted(fd, "+OK\r\n");
        } else if (numOfParts == 2 && strcasecmp("USER", pointers[0]) == 0 && authorization) { 
            if (is_valid_user(pointers[1], NULL)) {
                user = malloc(strlen(pointers[1]) + 1);
                memcpy(user, pointers[1], strlen(pointers[1]) + 1);
                send_formatted(fd, "+OK %s\r\n", user);
            } else {
                if (user != NULL) {
                    free(user);
                    user = NULL;
                }
                send_formatted(fd, "-ERR user %s not found\r\n", pointers[1]);
            }
        } else if (numOfParts == 2 && strcasecmp("PASS", pointers[0]) == 0 && user != NULL && authorization) {
            if (is_valid_user(user, pointers[1])) {
                authorization = 0;
                transaction = 1;
                userMail = load_user_mail(user);
                send_formatted(fd, "+OK maildrop locked and ready\r\n");
            } else {
                if (user != NULL) {
                    free(user);
                    user = NULL;
                }
                send_formatted(fd, "-ERR bad pass\r\n");
            }
        } else if (numOfParts == 1 && strcasecmp("STAT", pointers[0]) == 0 && transaction) {
            int num = get_mail_count(userMail, 0);
            int size = get_mail_list_size(userMail);
            send_formatted(fd, "+OK %d %d\r\n", num, size);
        } else if ((numOfParts == 2 || numOfParts == 1) && strcasecmp("LIST", pointers[0]) == 0 && transaction) {
            if (numOfParts == 2) {
                errno = 0;
                int pos = strtol(pointers[1], NULL, 10);
                //check that strtol returned successfully and that pos is within range for current user mail list
                if (errno == 0 && pos <= get_mail_count(userMail, 1) && pos > 0) {
                    mail_item_t iiq = get_mail_item(userMail, pos - 1);
                    if (iiq == NULL) {
                        send_formatted(fd, "-ERR no such message\r\n");
                        continue;
                    }
                    size_t size = get_mail_item_size(iiq);
                    send_formatted(fd, "+OK %d %zu\r\n", pos, size);
                } else {
                    send_formatted(fd, "-ERR no such message\r\n");
                }
            } else {
                int num = get_mail_count(userMail, 0);
                size_t si = get_mail_list_size(userMail);
                if (get_mail_count(userMail, 0) == 0) {
                    send_formatted(fd, "+OK 0 messages (0 octets)\r\n");
                    send_formatted(fd, ".\r\n");
                    continue;
                }
                send_formatted(fd, "+OK %d messages (%zu octets)\r\n", num, si);
                for (int i = 0; i < get_mail_count(userMail, 1); i++) {
                    mail_item_t iiq = get_mail_item(userMail, i);
                    if (iiq != NULL) {
                        size_t size = get_mail_item_size(iiq);
                        send_formatted(fd, "%d %zu\r\n", i + 1, size);
                    }
                }
                send_formatted(fd, ".\r\n");
            }
        } else if (numOfParts == 2 && strcasecmp("RETR", pointers[0]) == 0 && transaction) {
            errno = 0;
            int pos = strtol(pointers[1], NULL, 10);
            //check that strtol returned successfully and that pos is within range for current user mail list
            if (errno == 0 && pos <= get_mail_count(userMail, 1) && pos > 0) { 
                mail_item_t iiq = get_mail_item(userMail, pos - 1);
                    if (iiq == NULL) {
                        send_formatted(fd, "-ERR no such message\r\n");
                        continue;
                    }
                send_formatted(fd, "+OK message follows\r\n");
                FILE* fiq = get_mail_item_contents(iiq);
                //read mail item FILE and output to client
                while (fread(recvbuf, 1, sizeof recvbuf, fiq) > 0) {
                    send_formatted(fd, "%s", recvbuf);
                }
                //send .<CLRF> to end data transmission
                send_formatted(fd, ".\r\n");
                //reset recvbuf memory so as not to misoutput data from a previous RETR call
                memset(recvbuf, 0, sizeof recvbuf);
                fclose(fiq);
            } else {
                send_formatted(fd, "-ERR invalid\r\n");
            }
        } else if (numOfParts == 2 && strcasecmp("DELE", pointers[0]) == 0 && transaction) {
            errno = 0;
            int pos = strtol(pointers[1], NULL, 10);
            //check that strtol returned successfully and that pos is within range for current user mail list
            if (errno == 0 && pos <= get_mail_count(userMail, 1) && pos > 0) { 
                mail_item_t iiq = get_mail_item(userMail, pos - 1);
                    if (iiq == NULL) {
                        send_formatted(fd, "-ERR message already deleted\r\n");
                        continue;
                    }
                mark_mail_item_deleted(iiq);
                send_formatted(fd, "+OK msg deleted\r\n");
            } else {
                send_formatted(fd, "-ERR no such message\r\n");
            }
        } else if (numOfParts == 1 && strcasecmp("RSET", pointers[0]) == 0 && transaction) {
            int restored = reset_mail_list_deleted_flag(userMail);
            send_formatted(fd, "+OK %d messages restored\r\n", restored);
        } else {
            send_formatted(fd, "-ERR invalid syntax OR used at wrong time\r\n");
        }
    }
  
    nb_destroy(nb);
}
