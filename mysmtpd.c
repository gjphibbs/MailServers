#include "netbuffer.h"
#include "mailuser.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <ctype.h>

#define MAX_LINE_LENGTH 1024

static void handle_client(int fd);

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

    struct utsname my_uname;
    uname(&my_uname);

    user_list_t reverse_path = create_user_list();
    user_list_t forward_path = create_user_list();

    //track when requirements have been met
    int helo = 0;
    int mail = 0;
    int rcpt = 0;

    send_formatted(fd, "220 %s Simple Mail Transfer Service Ready\r\n", my_uname.__domainname);
    //begin polling for client input. End loop on client exit with quit or when nb_read_line states client is gone.
    while(1) {
        int ret = nb_read_line(nb, recvbuf);
        //check if client exited without QUIT
        if (ret == 0) {
            send_formatted(fd, "221 %s\r\n", my_uname.__domainname);
            break;
        }
        if (ret < 0) {
            send_formatted(fd, "221 %s\r\n", my_uname.__domainname);
            break;
        }
        //whitespace input and too-short inputs
        if (ret < 3) {
            send_formatted(fd, "500 Invalid\r\n");
            continue;
        }
        //split the args and store the number of args for guard clauses
        int numOfParts = split(recvbuf, pointers);
        if (numOfParts == 2 && (strcasecmp("EHLO", pointers[0]) == 0 || strcasecmp("HELO", pointers[0]) == 0)) {
            helo = 1;
            send_formatted(fd, "250 %s\r\n", my_uname.nodename);
        } else if (strcasecmp("QUIT", pointers[0]) == 0 && numOfParts == 1) {
            send_formatted(fd, "221 %s\r\n", my_uname.__domainname);
            break;
        } else if (strcasecmp("NOOP", pointers[0]) == 0 && numOfParts == 1) {
            send_formatted(fd, "250 OK\r\n");
        } else if (numOfParts == 2 && strcasecmp("MAIL", pointers[0]) == 0 && strncasecmp("FROM:<", pointers[1], 5) == 0) {
            if (helo == 0) {
                send_formatted(fd, "503 no helo\r\n");
                continue;
            }
            //remove previous sender. remove destinations
            destroy_user_list(reverse_path);
            destroy_user_list(forward_path);
            //argument format checks, while checking also extracts user from betwenn "<" and ">"
            char * user = strchr(pointers[1], '<');
            if (user == NULL) {
                send_formatted(fd, "500 Invalid\r\n");
                continue;
            }
            char * end = strchr(user, '>');
            if (end == NULL) {
                send_formatted(fd, "500 Invalid\r\n");
                continue;
            }
            *end = '\0';
            if (strlen(user) == 0) {
                send_formatted(fd, "500 Invalid\r\n");
                continue;
            }
            mail = 1;
            user = user + 1;
            add_user_to_list(&reverse_path, user);
            send_formatted(fd, "250 OK\r\n");
        } else if (numOfParts == 2 && strcasecmp("RCPT", pointers[0]) == 0 && strncasecmp("TO:<", pointers[1], 4) == 0) {
            if (mail == 0) {
                send_formatted(fd, "503 Invalid\r\n");
                continue;
            }
            char * user = strchr(pointers[1], '<');
            if (user == NULL) {
                send_formatted(fd, "500 Invalid\r\n");
                continue;
            }
            char * end = strchr(user, '>');
            if (end == NULL) {
                send_formatted(fd, "500 Invalid\r\n");
                continue;
            }
            *end = '\0';
            if (strlen(user) == 0) {
                send_formatted(fd, "500 Invalid\r\n");
                continue;
            }
            user = user + 1;
            if (is_valid_user(user, NULL)) {
                add_user_to_list(&forward_path, user);
                rcpt = 1;
                send_formatted(fd, "250 OK\r\n");
            } else {
                send_formatted(fd, "550 Invalid\r\n");
            }
        } else if (strcasecmp("VRFY", pointers[0]) == 0) {
            if (!(numOfParts == 2 || numOfParts == 3)) {
                send_formatted(fd, "501 Invalid\r\n");
                continue;
            }
            char * user = pointers[1];
            if (numOfParts == 3) {
                char * pass = pointers[2];
                if (is_valid_user(user, pass)) {
                    send_formatted(fd, "250 OK\r\n");
                } else {
                    send_formatted(fd, "550 does not exist\r\n");
                }
            } else {
                if (is_valid_user(user, NULL)) {
                    send_formatted(fd, "250 OK\r\n");
                } else {
                    send_formatted(fd, "550 does not exist\r\n");
                }
            }
        } else if (strcasecmp("RSET", pointers[0]) == 0 && numOfParts == 1) {
            //reset status except helo
            mail = 0;
            rcpt = 0;
            destroy_user_list(forward_path);
            destroy_user_list(reverse_path);
            forward_path = create_user_list();
            reverse_path = create_user_list();
            send_formatted(fd, "250 OK\r\n");
        } else if (strcasecmp("DATA", pointers[0]) == 0 && numOfParts == 1) {
            if (rcpt == 0) {
                send_formatted(fd, "503 no helo\r\n");
                continue;
            }
            send_formatted(fd, "354 start input\r\n");
            char filename[] = "fileXXXXXX";
            int fp = mkstemp(filename);
            //begin polling for email message until <CRLF>.<CRLF>
            while (1) {
                int rec = nb_read_line(nb, recvbuf);
                if (rec == 0) {
                    send_formatted(fd, "221 %s\r\n", my_uname.__domainname);
                    unlink(filename);
                    close(fp);
                    break;
                }
                if (rec < 0) {
                    send_formatted(fd, "221 %s\r\n", my_uname.__domainname);
                    unlink(filename);
                    close(fp);
                    break;
                }
                if (strcasecmp(".\r\n", recvbuf) == 0) {
                    //end polling when a line contains only a . followed by <CRLF>
                    save_user_mail(filename, forward_path);
                    unlink(filename);
                    close(fp);
                    mail = 0;
                    rcpt = 0;
                    destroy_user_list(forward_path);
                    destroy_user_list(reverse_path);
                    forward_path = create_user_list();
                    reverse_path = create_user_list();
                    send_formatted(fd, "250 OK\r\n");
                    break;
                }
                if (strncasecmp(".", recvbuf, 1) == 0) {
                    write(fp, recvbuf + 1, rec - 1);
                    continue;
                }
                write(fp, recvbuf, rec);
            }
        } else if (strcasecmp("EXPN", pointers[0]) == 0) {
            send_formatted(fd, "502 unsupported\r\n");
        } else if (strcasecmp("HELP", pointers[0]) == 0) {
            send_formatted(fd, "502 unsupported\r\n");
        } else {
            send_formatted(fd, "500 invalid\r\n");
        }
    }
    nb_destroy(nb);
    return;
}
