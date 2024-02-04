#include <arpa/inet.h>
#include <fcntl.h>
#include <ndbm.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>

#define BUF_SIZE 9000
#define SMALL_BUF 9000

void  error_handling(char *message);
void *request_handler(void *arg);
void  send_error(FILE *fp);
void  send_data(FILE *fp, char *ct, char *file_name);
char *content_type(char *file);
void  handle_post_request(FILE *clnt_read, int content_length, DBM *db);

int main(int argc, char *argv[])
{
    int                serv_sock, clnt_sock;
    struct sockaddr_in serv_adr, clnt_adr;
    socklen_t               clnt_adr_size;
    char               buf[BUF_SIZE];

    pthread_t t_id;

    if(argc != 2)
    {
        printf("Usage : %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // create tcp socket
    serv_sock = socket(PF_INET, SOCK_STREAM, 0);

    // initialize server address info
    memset(&serv_adr, 0, sizeof(serv_adr));
    serv_adr.sin_family      = AF_INET;
    serv_adr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_adr.sin_port        = htons(atoi(argv[1]));

    // allocate the address
    if(bind(serv_sock, (struct sockaddr *)&serv_adr, sizeof(serv_adr)) == -1)
        error_handling("bind");

    // listening
    if(listen(serv_sock, SOMAXCONN) == -1)
        error_handling("listen() error");

    while(1)
    {
        clnt_adr_size = sizeof(clnt_adr);
        clnt_sock     = accept(serv_sock, (struct sockaddr *)&clnt_adr, &clnt_adr_size);
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(clnt_adr.sin_addr), client_ip, INET_ADDRSTRLEN);
        printf("Connection Request: %s:%d\n", client_ip, ntohs(clnt_adr.sin_port));

        pthread_create(&t_id, NULL, request_handler, &clnt_sock);
        //pthread_detach(t_id);

    }
    close(serv_sock);
    return 0;
}

void error_handling(char *message)
{
    fputs(message, stderr);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

void *request_handler(void *arg)
{
    int   clnt_sock = *((int *)arg);
    char  req_line[SMALL_BUF]; //get info
    char  req_contents[SMALL_BUF];
    FILE *clnt_read;
    FILE *clnt_write;

    char method[10];
    char ct[15];
    char file_name[30];

    clnt_read  = fdopen(clnt_sock, "r");
    clnt_write = fdopen(dup(clnt_sock), "w");



    // Read the first line of the request
    fgets(req_line, SMALL_BUF, clnt_read);
    //printf("req_line Line: %s\n", req_line);


    int content_length = 0;
    // Read the headers until an empty line is encountered
    while (fgets(req_contents, SMALL_BUF, clnt_read) != NULL) {

        //printf("Request Line: %s\n", req_contents);
        // 요청 헤더의 끝을 만나면 req_contents
        if (strcmp(req_contents, "\r\n") == 0 || strcmp(req_contents, "\n") == 0) {
            break;
        }

        // Find the Content-Length header
        if (strstr(req_contents, "Content-Length:") != NULL) {
            content_length = atoi(req_contents + strlen("Content-Length:"));

        }
    }

    //printf("Content-Length: %d\n", content_length);



    if(strstr(req_line, "HTTP/") == NULL)
    {
        send_error(clnt_write);
        fclose(clnt_read);
        fclose(clnt_write);
        return NULL;
    }

    strcpy(method, strtok(req_line, " /"));
    if(strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0 && strcmp(method, "POST") != 0)
    {
        send_error(clnt_write);
        fclose(clnt_read);
        fclose(clnt_write);
        return NULL;
    }

    printf("method 값: %s\n", method);

    strcpy(file_name, strtok(NULL, " /"));
    strcpy(ct, content_type(file_name));


    if(strcmp(method, "POST") == 0)
    {
        // Handle POST request
        // Read POST data from clnt_read
        // Process the data if needed

        // data base-----------------------------
        DBM   *db;
        int    dbm_flags = O_RDWR | O_CREAT;
        mode_t dbm_mode  = 0666;

        // 데이터베이스 열기
        db = dbm_open("post_data.db", dbm_flags, dbm_mode);
        if(!db)
        {
            fprintf(stderr, "Failed to open the database\n");
            exit(EXIT_FAILURE);
        }

        //----------------------------------------------------------------
        handle_post_request(clnt_read, content_length, db);


        // 데이터베이스 닫기
        dbm_close(db);
        fclose(clnt_read);


    }

    if(strcmp(method, "HEAD") == 0)
    {

    }


    send_data(clnt_write, ct, file_name);

    close(clnt_sock);
    pthread_exit(NULL);

}

void handle_post_request(FILE *clnt_read, int content_length, DBM *db)
{

    // content_length 출력
    printf("Content Length: %d\n", content_length);
    char *post_data = (char *)malloc(content_length + 1);
    if (post_data == NULL) {
        fprintf(stderr, "Failed to allocate memory for post data\n");
        exit(EXIT_FAILURE);
    }

    // 데이터 읽기
    size_t bytes_read = fread(post_data, sizeof(char), content_length, clnt_read);
    if (bytes_read != content_length) {
        fprintf(stderr, "Failed to read data from client\n");
        exit(EXIT_FAILURE);
    }
    post_data[content_length] = '\0';    // Null terminate the string
    printf("Value: %s\n", post_data);
    // post_data를 키와 값으로 분리
    char *key_str = strtok(post_data, "=");
    char *value_str = strtok(NULL, "=");

    // key_str과 value_str에는 각각 "post_data_key"와 "this+is+what%3F"가 저장됩니다.

    // 키와 값 설정
    datum key, value;
    key.dptr = key_str;
    key.dsize = strlen(key_str);
    value.dptr = value_str;
    value.dsize = strlen(value_str);
//
//    // 데이터베이스에 저장
//    if (dbm_store(db, key, value, DBM_INSERT) != 0) {
//        fprintf(stderr, "Failed to store data in the database: %d\n", dbm_error(db));
//        free(post_data); // Free allocated memory
//        exit(EXIT_FAILURE);
//    }

    value = dbm_fetch(db, key);

    if (value.dptr == NULL) {
        printf("Key not found in the database.\n");
    } else {
        printf("Value: %s\n", (char *)value.dptr);

    }
    free(post_data); // Free allocated memory


}

void send_data(FILE *fp, char *ct, char *file_name)
{
    char  protocol[] = "HTTP/1.0 200 OK\r\n";
    char  server[]   = "Server: Simple HTTP Server\r\n";
    char  cnt_len[SMALL_BUF];
    char  cnt_type[SMALL_BUF];
    char  buf[BUF_SIZE];
    FILE *send_file;

    // Send the HTTP response header
    sprintf(cnt_type, "Content-type:%s\r\n\r\n", ct);
    printf("File Path: %s\n", file_name);

    send_file = fopen(file_name, "r");
    if(send_file == NULL)
    {
        perror("fopen");
        // 파일이 없는 경우에 대한 처리
        send_file = fopen("404.html", "r");
        if(send_file == NULL) {
            perror("404.html fopen");
            // 대체 파일을 찾을 수 없는 경우에 대한 처리
            exit(EXIT_FAILURE); // 또는 다른 적절한 처리 방법 사용
        }

    }
    // header info
    fputs(protocol, fp);
    fputs(server, fp);
    fputs(cnt_len, fp);
    fputs(cnt_type, fp);

    // Send the content of the requested file
    while(fgets(buf, BUF_SIZE, send_file) != NULL)
    {
        fputs(buf, fp);
        fflush(fp);
    }
    fclose(send_file);
    fflush(fp);
    fclose(fp);
}

void send_error(FILE *fp)
{
    char protocol[] = "HTTP/1.0 400 Bad Request\r\n";
    char server[]   = "Server: Simple HTTP Server\r\n";
    char cnt_len[]  = "Content-length:2048\r\n";
    char cnt_type[] = "Content-type:text/html\r\n\r\n";
    char content[]  = "<html><head><title>NETWORK</title></head>"
                      "<body><font size=+5><br>Whoops, something went wrong!</font>"
                      "</body></html>";

    fputs(protocol, fp);
    fputs(server, fp);
    fputs(cnt_len, fp);
    fputs(cnt_type, fp);
    fputs(content, fp);
    fflush(fp);
}

char *content_type(char *file)
{
    char extension[SMALL_BUF];
    char file_name[SMALL_BUF];
    strcpy(file_name, file);
    strtok(file_name, ".");
    strcpy(extension, strtok(NULL, "."));

    if(strcmp(extension, "html") == 0 || strcmp(extension, "htm") == 0)
        return "text/html";
    else if(strcmp(extension, "jpg") == 0 || strcmp(extension, "jpeg") == 0)
        return "image/jpeg";
    else if(strcmp(extension, "gif") == 0)
        return "image/gif";
    else if(strcmp(extension, "ico") == 0)
        return "image/x-icon";
    else
        return "text/plain";


}
