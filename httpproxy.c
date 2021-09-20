#include <err.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netdb.h>
#include <ctype.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#define __USE_XOPEN
#include <time.h>

struct cache_file {
    char *buffer;
    char name[16];
    char time[30];
    int lru;
    struct cache_file* next;
};

struct cache_file* cache_head = NULL;
struct cache_file* cache_tail = NULL;
int num_cached_files = 0;

/**
   Converts a string to an 16 bits unsigned integer.
   Returns 0 if the string is malformed or out of the range.
 */
uint16_t strtouint16(char number[]) {
    char *last;
    long num = strtol(number, &last, 10);
    if (num <= 0 || num > UINT16_MAX || *last != '\0') {
        return 0;
    }
    return num;
}

/**
   Creates a socket for listening for connections.
   Closes the program and prints an error message on error.
*/
int create_listen_socket(uint16_t port) {
    struct sockaddr_in addr;
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        err(EXIT_FAILURE, "socket error");
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htons(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(listenfd, (struct sockaddr*)&addr, sizeof addr) < 0) {
        err(EXIT_FAILURE, "bind error");
    }
    if (listen(listenfd, 500) < 0) {
        err(EXIT_FAILURE, "listen error");
    }
    return listenfd;
}

int create_client_socket(uint16_t port) {
    int clientfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    if (clientfd < 0) {
      return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (connect(clientfd, (struct sockaddr*) &addr, sizeof addr)) {
      return -1;
    }
    return clientfd;
}

int get_content_length(char *buffer) {
    char *ptr = strstr(buffer, "Content-Length: ");
    if(ptr != NULL){
        return atoi(ptr+16);
    } else {
        return 0;
    }
}

char* get_mod_time(char *buffer) {
    char *ptr = strstr(buffer, "Last-Modified: ");
    if(ptr != NULL){
        return ptr + 15;
    }
    return NULL;
}

struct cache_file* get_cache_file(char *filename){
    struct cache_file *cf;
    cf = cache_head;
    while(cf != NULL){
        if(memcmp(cf->name, filename, 15) == 0){
            return cf;
        }
        cf = cf->next;
    }
    return cf;
}

void add_cache_file(struct cache_file *cf){
    if(cache_head == NULL){
        cache_head = cf;
        cache_tail = cf;
        cf->next = NULL;
    } else {
        cf->next = cache_head;
        cache_head = cf;
    }
    num_cached_files++;
}

void remove_cache_file(char *filename){
    struct cache_file *cur = cache_head;
    struct cache_file *prev = cache_head;

    while(cur != NULL){
        if(strcmp(cur->name, filename) == 0){
            if(cur == cache_head && cur == cache_tail){
                cache_head = NULL;
                cache_tail = NULL;
                free(cur->buffer);
                free(cur);
                num_cached_files--;
                break;
            }
            if(cur == cache_head){
                cache_head = cur->next;
                free(cur->buffer);
                free(cur);
                num_cached_files--;
                break;
            }
            if(cur == cache_tail){
                cache_tail = prev;
                free(cur->buffer);
                free(cur);
                num_cached_files--;
                break;
            }
            prev->next = cur->next;
            free(cur->buffer);
            free(cur);
            num_cached_files--;
            break;
        }
        prev = cur;
        cur = cur->next;
    }
}


void replace_lru(struct cache_file *cf){
    struct cache_file *temp = cache_head;
    struct cache_file *prev = cache_head;
    int max = 0;

    while(temp != NULL){
        if(temp->lru > max){
            max = temp->lru;
        }
        temp = temp->next;
    }
    prev = cache_head;
    temp = cache_head;

    while(temp != NULL){
        if(temp->lru == max){
            if(temp == cache_head){
              cache_head = cf;
            }
            if(temp == cache_tail){
              cache_tail = cf;
            }
            cf->next = temp->next;
            prev->next = cf;
            free(temp->buffer);
            free(temp);
            break;
        }
        prev = temp;
        temp = temp->next;
    }
}

void replace_fifo(struct cache_file *cf){
    struct cache_file *temp = cache_head;

    while(temp != NULL){
        if(temp->next == cache_tail){
            free(cache_tail->buffer);
            free(cache_tail);
            cache_tail = temp;
            temp->next = NULL;
            break;
        }
        temp = temp->next;
    }
    cf->next = cache_head;
    cache_head = cf;
}

void update_lru(){
    struct cache_file *cf;
    cf = cache_head;
    while(cf != NULL){
        cf->lru++;
        cf = cf->next;
    }
}

/*This function is unfortunately never called, as the cache is kept open during
  the entire time the program is running
void free_cache(){
    struct cache_file *cf;
    while(cache_head != NULL){
        cf = cache_head;
        cache_head = cache_head->next;
        free(cf->buffer);
        free(cf);
    }
    num_cached_files = 0;
    cache_head = NULL;
    cache_tail = NULL;
}*/

int recv_header(int connfd, char *buffer){
    int status;
    char *ptr;
    int bytes_read = 0;
    memset(buffer, 0, 4096);
    while(1){
        status = recv(connfd, buffer + bytes_read, 1, 0);
        if(status <= 0) return -1;
        bytes_read += status;
        if(bytes_read > 4096) return -1;
        ptr = strstr(buffer, "\r\n\r\n");
        if (ptr != NULL) break;
    }
    return bytes_read;
}

int body_to_buffer(int recvfd, char *buffer, int content_len){
    int status;
    int bytes_read = 0;
    memset(buffer, 0, content_len);
    while(bytes_read < content_len){
        if(content_len - bytes_read > 4096) {
            status = recv(recvfd, buffer + bytes_read, 4096, 0);
        } else {
            status = recv(recvfd, buffer + bytes_read, content_len - bytes_read, 0);
        }
        if(status <= 0) return -1;
        bytes_read += status;
        if(bytes_read > content_len) return -1;
    }
    return EXIT_SUCCESS;
}

int relay_body(int recvfd, int sendfd, char *buffer, int content_len){
    int status;
    int bytes_read = 0;
    memset(buffer, 0, 4096);
    while(bytes_read < content_len){
        if((content_len - bytes_read) >= 4096) {
            status = recv(recvfd, buffer, 4096, 0);
        } else {
            status = recv(recvfd, buffer, content_len - bytes_read, 0);
        }
        if(status <= 0) return -1;
        bytes_read += status;
        if(bytes_read > content_len) return -1;
        send(sendfd, buffer, status, 0);
    }
    return EXIT_SUCCESS;
}

void handle_connection(int connfd, int serverfd, int max_cache_files, int max_file_size, char lru) {
    char buffer1[4096];
    char buffer2[4096];
    char head_request[128];
    char get_response[128];
    char request_type[16];
    char response_code[4];
    char filename[16];
    char time[30];
    int bytes_read = 0;
    int status = 0;
    int content_len = 0;
    struct cache_file *cf = NULL;
    char hostbuffer[256];
    char *IPbuffer;
    struct hostent *host_entry;
    gethostname(hostbuffer, sizeof(hostbuffer));
    host_entry = gethostbyname(hostbuffer);
    IPbuffer = inet_ntoa(*((struct in_addr*) host_entry->h_addr_list[0]));

    while(1){
        bytes_read = recv_header(connfd, buffer1);
        //printf("recieved client request\n");
        if(bytes_read <= 0) {close(connfd); return;}
        status = sscanf(buffer1,"%s %*c%s", request_type, filename);
        if (status == -1) {close(connfd); return;}

        content_len = get_content_length(buffer1);
        if((!strcmp("GET", request_type)) && (content_len <= max_file_size) && (max_cache_files != 0)){
            //printf("HANDLING GET REQUEST\n\n");
            cf = get_cache_file(filename);
            if(cf == NULL){
                /**GET THE FILE,
                CACHE THE FILE*/
                cf = (struct cache_file*)malloc(sizeof(struct cache_file));
                memcpy(cf->name, filename, 15);
                cf->next = NULL;
                status = send(serverfd, buffer1, bytes_read, 0);
                if (status == -1) {close(connfd); return;}
                status = recv_header(serverfd, buffer1);
                if (status == -1) {close(connfd); return;}
                sscanf(buffer1,"%*s %s", response_code);
                if(strcmp(response_code, "404") == 0){
                    status = send(connfd, buffer1, status, 0);
                    if (status == -1) {close(connfd); return;}
                    content_len = get_content_length(buffer1);
                    relay_body(serverfd, connfd, buffer1, content_len);
                    continue;
                }
                if(strcmp(response_code, "400") == 0){
                    //printf("removing file from cache!\n");
                    status = send(connfd, buffer1, status, 0);
                    if (status == -1) {close(connfd); return;}
                    close(connfd);
                    continue;;
                }
                memcpy(cf->time, get_mod_time(buffer1), 29);
                content_len = get_content_length(buffer1);
                cf->buffer = (char*)malloc(content_len*sizeof(char));
                status = send(connfd, buffer1, status, 0);
                if (status == -1) {close(connfd); return;}
                status = body_to_buffer(serverfd, cf->buffer, content_len);
                if (status == -1) {close(connfd); return;}
                status = send(connfd, cf->buffer, content_len, 0);
                if (status == -1) {close(connfd); return;}
                cf->lru = 0;
                update_lru();
                /**IF CACHE IS NOT FULL, REPLACEMENT POLICY IS IRRELEVANT*/
                if(num_cached_files != max_cache_files){
                    add_cache_file(cf);
                } else if(lru) {
                    /**IF CACHE IS FULL,
                        REPLACE LEAST RECENTLY USED*/
                    replace_lru(cf);
                } else {
                    /**IF CACHE IS FULL,
                        USE FIRST IN FIRST OUT*/
                    replace_fifo(cf);
                }
                continue;
            }
            sprintf(head_request, "HEAD /%s HTTP/1.1\r\nHost: %s\r\n\r\n", filename, IPbuffer);
            status = send(serverfd, head_request, strlen(head_request), 0);
            if (status == -1) {close(connfd); return;}
            status = recv_header(serverfd, buffer2);
            if(status == -1) {close(connfd); return;}
            content_len = get_content_length(buffer2);
            sscanf(buffer2,"%*s %s", response_code);
            if(strcmp(response_code, "404") == 0){
                //printf("removing file from cache!\n");
                remove_cache_file(filename);
                status = send(connfd, buffer2, strlen(buffer2), 0);
                if(status == -1) {close(connfd); return;}
                relay_body(serverfd, connfd, buffer2, content_len);
                continue;
            }
            if(strcmp(response_code, "400") == 0){
                //printf("removing file from cache!\n");
                remove_cache_file(filename);
                status = send(connfd, buffer2, strlen(buffer2), 0);
                if(status == -1) {close(connfd); return;}
                relay_body(serverfd, connfd, buffer2, content_len);
                return;
            }

            memcpy(time, get_mod_time(buffer2), 29);
            /**GET FILE EXISTS IN CACHE
            REPLACE IF IT IS OUTDATED
            OTHERWISE USE CACHE*/
            if (cf->lru != 0){
                update_lru();
                cf->lru = 0;
            }
            if(memcmp(cf->time, time, 29) != 0){
                //printf("file outdated, updating cache\n");
                /**OUTDATED:
                    GET FILE FROM SERVER
                    UPDATE THE CACHE, AND SEND*/
                status = send(serverfd, buffer1, bytes_read, 0);
                if (status == -1) {close(connfd); return;}
                status = recv_header(serverfd, buffer1);
                if (status == -1) {close(connfd); return;}
                content_len = get_content_length(buffer1);
                free(cf->buffer);
                cf->buffer = (char*)malloc(content_len);
                memcpy(cf->time, time, 29);
                status = send(connfd, buffer1, status, 0);
                if (status == -1) {close(connfd); return;}
                status = body_to_buffer(serverfd, cf->buffer, content_len);
                if (status == -1) {close(connfd); return;}
                status = send(connfd, cf->buffer, content_len, 0);
                if (status == -1) {close(connfd); return;}
            } else {
                //printf("using cache\n");
                /*NOT OUTDATED:
                  NO NEED TO COMMUNICATE WITH SERVER
                  USE CACHE TO SEND*/
                sprintf(get_response, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nHost: %s\r\n\r\n", content_len, IPbuffer);
                status = send(connfd, get_response, strlen(get_response), 0);
                if (status == -1) {close(connfd); return;}
                status = send(connfd, cf->buffer, content_len, 0);
                if (status == -1) {close(connfd); return;}
            }
            continue;
        }
        /*NOT A GET REQUEST, OR FILE IS TOO BIG TO USE CACHE
        DO NOT USE CACHE*/
        if(strcmp("GET", request_type) == 0){
            remove_cache_file(filename);
            status = send(serverfd, buffer1, bytes_read, 0);
            if (status == -1) return;
            status = relay_body(connfd, serverfd, buffer1, content_len);
            if (status == -1) {close(connfd); return;}
            status = recv_header(serverfd, buffer1);
            if (status == -1) {close(connfd); return;}
            content_len = get_content_length(buffer1);
            relay_body(serverfd, connfd, buffer1, content_len);
        }
        if(strcmp("PUT", request_type) == 0){
            remove_cache_file(filename);
            status = send(serverfd, buffer1, bytes_read, 0);
            if (status == -1) return;
            status = relay_body(connfd, serverfd, buffer1, content_len);
            if (status == -1) {close(connfd); return;}
            status = recv_header(serverfd, buffer1);
            if (status == -1) {close(connfd); return;}
            status = send(connfd, buffer1, status, 0);
            if (status == -1) {close(connfd); return;}
            content_len = get_content_length(buffer1);
            relay_body(serverfd, connfd, buffer1, content_len);
        }
        if(strcmp("HEAD", request_type) == 0){
            remove_cache_file(filename);
            status = send(serverfd, buffer1, bytes_read, 0);
            if (status == -1) return;
            status = relay_body(connfd, serverfd, buffer1, content_len);
            if (status == -1) {close(connfd); return;}
            status = recv_header(serverfd, buffer1);
            if (status == -1) {close(connfd); return;}
            status = send(connfd, buffer1, status, 0);
            if (status == -1) {close(connfd); return;}
        }
        //MAKING ASSUMTION THAT SERVER ONLY SENDS BODY FOR A PUT REQUEST*/
    }
}

void usage(){
    errx(1, "Usage: httpproxy [client port] [server port] [-cmu] [file...]");
}

int main(int argc, char *argv[]) {
    int listenfd;
    int serverfd;
    int connfd;
    uint16_t client_port = 0;
    uint16_t server_port = 0;
    int cache_cap = 3;
    int file_max = 65536;
    char lru = 0;
    int index = 1;

    while (index < argc) {
        if( strcmp(argv[index], "-u") == 0){
            lru = 1;
            index++;
            continue;
        } else if(strcmp(argv[index], "-c") == 0){
            index++;
            if(index >= argc) usage();
            cache_cap = atoi(argv[index]);
            if(cache_cap < 0) usage();
            index++;
            continue;
        } else if(strcmp(argv[index], "-m") == 0){
            index++;
            if(index >= argc) usage();
            file_max = atoi(argv[index]);
            if(file_max == 0) usage();
            index++;
            continue;
        }
        if (!client_port){
            client_port = strtouint16(argv[index]);
            if(client_port == 0) usage();
        }else if(!server_port){
            server_port = strtouint16(argv[index]);
            if(client_port == 0) usage();
        }else{
            usage();
        }
        index++;
    }
    if(client_port == 0 || server_port == 0){
        usage();
    }
    listenfd = create_listen_socket(client_port);
    serverfd = create_client_socket(server_port);
    while(1) {
        connfd = accept(listenfd, NULL, NULL);
        if (connfd < 0) {
            warn("accept error");
            continue;
        }
        if(serverfd < 0) {
            serverfd = create_client_socket(server_port);
            if(serverfd < 0){
                errx(1, "Unable to connect with server.\n");
            }
        }
        handle_connection(connfd, serverfd, cache_cap, file_max, lru);
        //connection was lost
    }
    /*  UNREACHABLE  */
    return EXIT_SUCCESS;
}
