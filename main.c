#include <stdio.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>  // Include this header for inet_ntop
#include <pthread.h>
#include <unistd.h>

#define BUF_SIZE 1024

#define SMALL_BUF 100

void error_handling(char* message);
void* request_handler(void *arg);

int main(int argc, char *argv[]) {
    int serv_sock, clnt_sock;
    struct sockaddr_in serv_adr, clnt_adr;
    int clnt_adr_size;
    char buf[BUF_SIZE];

    pthread_t t_id;

    if (argc != 2) {
        printf("Usage : %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    //create tcp socket
    serv_sock = socket(PF_INET, SOCK_STREAM, 0);

    //initialize server address info
    memset(&serv_adr, 0, sizeof(serv_adr));
    serv_adr.sin_family = AF_INET;
    serv_adr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_adr.sin_port = htons(atoi(argv[1]));

    //allocate the address
    if (bind(serv_sock, (struct sockaddr*)&serv_adr, sizeof(serv_adr)) == -1)
        error_handling("bind");

    //listening
    if (listen(serv_sock, SOMAXCONN) == -1)
        error_handling("listen() error");

    while (1) {
        clnt_adr_size = sizeof(clnt_adr);
        clnt_sock = accept(serv_sock, (struct sockaddr*)&clnt_adr, &clnt_adr_size);
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(clnt_adr.sin_addr), client_ip, INET_ADDRSTRLEN);
        printf("Connection Request: %s:%d\n", client_ip, ntohs(clnt_adr.sin_port));

        pthread_create(&t_id, NULL, request_handler, &clnt_sock);
        pthread_detach(t_id);
    }
    close(serv_sock);
    return 0;
}

void error_handling(char* message) {
    perror(message);
    exit(EXIT_FAILURE);
}

void* request_handler(void *arg){
    int clnt_sock = *((int*)arg);
    char req_line[SMALL_BUF];
    FILE* clnt_read;
    FILE* clnt_write;

    char method[10];
    char ct[15];
    char file_name[30];




}
