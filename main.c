#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ndbm.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>    // noreturn 헤더 파일 포함
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define THREAD_POOL_SIZE 4     // 스레드 풀 크기
#define TASK_QUEUE_SIZE 100    // 작업 큐 크기
#define BUF_SIZE 9000
#define SMALL_BUF 1024
#define base 10
#define DBM_MODE 0666
#define GDBM 512

#define magic1 10
#define magic2 15
#define magic3 30

// 작업 구조체 정의
typedef struct
{
    void (*function)(void *);    // 작업 함수 포인터
    void *argument;              // 작업 인자
    int   completed;             // 작업 완료 여부 플래그
} Task;

// 스레드 풀 구조체 정의
typedef struct
{
    pthread_t       threads[THREAD_POOL_SIZE];      // 스레드 배열
    Task            task_queue[TASK_QUEUE_SIZE];    // 작업 큐
    int             queue_front;                    // 큐의 맨 앞 인덱스
    int             queue_rear;                     // 큐의 맨 뒤 인덱스
    pthread_mutex_t queue_mutex;                    // 큐에 대한 뮤텍스
    pthread_cond_t  queue_not_empty;                // 작업이 들어올 때까지 대기하는 조건 변수
    pthread_cond_t  queue_not_full;                 // 작업 큐가 가득 차면 대기하는 조건 변수
    int             shutdown;                       // 스레드 풀 종료 여부
    int             active_tasks;                   // 실행 중인 작업 수
    pthread_mutex_t active_tasks_mutex;             // 실행 중인 작업 수에 대한 뮤텍스
    pthread_cond_t  all_tasks_completed;            // 모든 작업이 완료될 때까지 대기하는 조건 변수
} ThreadPool;

noreturn void  error_handling(const char *message);
void           request_handler(void *arg);
void           send_error(FILE *fp);
void           send_data(FILE *fp, char *ct, char *file_name);
const char    *content_type(const char *file);
noreturn void *thread_function(void *arg);
void           thread_pool_init(ThreadPool *pool);
void           thread_pool_add_task(ThreadPool *pool, void (*function)(void *), void *argument);
void           thread_pool_wait_all_tasks_completed(ThreadPool *pool);
void           thread_pool_shutdown(ThreadPool *pool);
void           test_task_function(void *arg);
void           handle_post_request(FILE *clnt_read, int content_length, GDBM_FILE db);

// 스레드 함수
noreturn void *thread_function(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;
    Task        task;

    while(1)
    {
        pthread_mutex_lock(&(pool->queue_mutex));

        // 작업 큐가 비어있을 때까지 대기
        while((pool->queue_front == pool->queue_rear) && !(pool->shutdown))
        {
            pthread_cond_wait(&(pool->queue_not_empty), &(pool->queue_mutex));
        }

        // 스레드 풀 종료 조건 확인
        if(pool->shutdown)
        {
            pthread_mutex_unlock(&(pool->queue_mutex));
            pthread_exit(NULL);
        }

        // 작업 큐에서 작업 가져오기
        task              = pool->task_queue[pool->queue_front];
        pool->queue_front = (pool->queue_front + 1) % TASK_QUEUE_SIZE;

        // 작업 큐가 비어있음을 통지
        pthread_cond_signal(&(pool->queue_not_full));
        pthread_mutex_unlock(&(pool->queue_mutex));

        // 작업 실행
        (*(task.function))(task.argument);

        // 작업 완료 플래그 설정
        pthread_mutex_lock(&(pool->active_tasks_mutex));
        task.completed = 1;
        pool->active_tasks--;

        // 모든 작업이 완료됐는지 확인하고 통지
        if(pool->active_tasks == 0)
        {
            pthread_cond_signal(&(pool->all_tasks_completed));
        }
        pthread_mutex_unlock(&(pool->active_tasks_mutex));
    }

    pthread_exit(NULL);
}

// 스레드 풀 초기화 함수
void thread_pool_init(ThreadPool *pool)
{
    int i;

    // 큐 인덱스 초기화
    pool->queue_front = 0;
    pool->queue_rear  = 0;

    // 뮤텍스 초기화
    pthread_mutex_init(&(pool->queue_mutex), NULL);
    pthread_cond_init(&(pool->queue_not_empty), NULL);
    pthread_cond_init(&(pool->queue_not_full), NULL);
    pthread_mutex_init(&(pool->active_tasks_mutex), NULL);
    pthread_cond_init(&(pool->all_tasks_completed), NULL);

    // 스레드 풀 생성
    for(i = 0; i < THREAD_POOL_SIZE; ++i)
    {
        pthread_create(&(pool->threads[i]), NULL, thread_function, (void *)pool);
    }
}

// 작업 추가 함수
void thread_pool_add_task(ThreadPool *pool, void (*function)(void *), void *argument)
{
    pthread_mutex_lock(&(pool->queue_mutex));

    // 작업 큐가 가득 찰 때까지 대기
    while(((pool->queue_rear + 1) % TASK_QUEUE_SIZE == pool->queue_front))
    {
        pthread_cond_wait(&(pool->queue_not_full), &(pool->queue_mutex));
    }

    // 작업 추가
    pool->task_queue[pool->queue_rear].function  = function;
    pool->task_queue[pool->queue_rear].argument  = argument;
    pool->task_queue[pool->queue_rear].completed = 0;    // 작업이 아직 완료되지 않았음을 표시
    pool->queue_rear                             = (pool->queue_rear + 1) % TASK_QUEUE_SIZE;
    pool->active_tasks++;    // 실행 중인 작업 수 증가

    // 작업이 들어왔음을 통지
    pthread_cond_signal(&(pool->queue_not_empty));
    pthread_mutex_unlock(&(pool->queue_mutex));
}

// 모든 작업이 완료될 때까지 대기
void thread_pool_wait_all_tasks_completed(ThreadPool *pool)
{
    pthread_mutex_lock(&(pool->active_tasks_mutex));
    while(pool->active_tasks > 0)
    {
        pthread_cond_wait(&(pool->all_tasks_completed), &(pool->active_tasks_mutex));
    }
    pthread_mutex_unlock(&(pool->active_tasks_mutex));
}

// 스레드 풀 종료 함수
void thread_pool_shutdown(ThreadPool *pool)
{
    int i;

    // 스레드 풀 종료 플래그 설정
    pool->shutdown = 1;

    // 작업이 들어왔음을 통지
    pthread_cond_broadcast(&(pool->queue_not_empty));

    // 스레드 조인
    for(i = 0; i < THREAD_POOL_SIZE; ++i)
    {
        pthread_join(pool->threads[i], NULL);
    }

    // 뮤텍스 및 조건 변수 해제
    pthread_mutex_destroy(&(pool->queue_mutex));
    pthread_cond_destroy(&(pool->queue_not_empty));
    pthread_cond_destroy(&(pool->queue_not_full));
    pthread_mutex_destroy(&(pool->active_tasks_mutex));
    pthread_cond_destroy(&(pool->all_tasks_completed));
}

int main(int argc, char *argv[])
{
    int                serv_sock;
    int                clnt_sock;
    struct sockaddr_in serv_adr;
    struct sockaddr_in clnt_adr;
    socklen_t          clnt_adr_size;
    ThreadPool         pool;

    // 스레드 풀 초기화
    thread_pool_init(&pool);

    if(argc != 2)
    {
        printf("Usage : %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // create tcp socket
    serv_sock = socket(PF_INET, SOCK_STREAM, 0);

    char    *endptr;
    long int port_num = strtol(argv[1], &endptr, base);

    if(*endptr != '\0')
    {
        error_handling("conversion error");
    }

    serv_adr.sin_port = htons((uint16_t)port_num);
    // initialize server address info
    memset(&serv_adr, 0, sizeof(serv_adr));
    serv_adr.sin_family      = AF_INET;
    serv_adr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_adr.sin_port        = htons((uint16_t)port_num);

    // allocate the address
    if(bind(serv_sock, (struct sockaddr *)&serv_adr, sizeof(serv_adr)) == -1)
    {
        error_handling("bind");
    }

    // listening
    if(listen(serv_sock, SOMAXCONN) == -1)
    {
        error_handling("listen() error");
    }

    while(1)
    {
        clnt_adr_size = sizeof(clnt_adr);
        clnt_sock     = accept(serv_sock, (struct sockaddr *)&clnt_adr, &clnt_adr_size);
        char client_ip[INET_ADDRSTRLEN];

        inet_ntop(AF_INET, &(clnt_adr.sin_addr), client_ip, INET_ADDRSTRLEN);
        printf("Connection Request: %s\n", client_ip);

        thread_pool_add_task(&pool, request_handler, &clnt_sock);
    }
    // 모든 작업이 완료될 때까지 대기
    thread_pool_wait_all_tasks_completed(&pool);

    // 스레드 풀 종료
    thread_pool_shutdown(&pool);

    close(serv_sock);
    return 0;
}

noreturn void error_handling(const char *message)
{
    fputs(message, stderr);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

// 테스트용 작업 함수
void test_task_function(void *arg)
{
    int *num = (int *)arg;
    printf("Task with argument: %d\n", *num);
}

void request_handler(void *arg)
{
    int clnt_sock = *((int *)arg);

    char  req_line[SMALL_BUF];
    char  req_contents[SMALL_BUF];
    FILE *clnt_read;
    FILE *clnt_write;

    char method[magic1];
    char ct[magic2];
    char file_name[magic3];

    clnt_read  = fdopen(clnt_sock, "r");
    clnt_write = fdopen(fcntl(clnt_sock, F_DUPFD_CLOEXEC, 0), "w");

    // Read the first line of the request
    fgets(req_line, SMALL_BUF, clnt_read);

    int content_length = 0;
    while(fgets(req_contents, SMALL_BUF, clnt_read) != NULL)
    {
        if(strcmp(req_contents, "\r\n") == 0 || strcmp(req_contents, "\n") == 0)
        {
            break;
        }
        // Find the Content-Length header
        if(strstr(req_contents, "Content-Length:") != NULL)
        {
            char *endptr;
            content_length = (int)strtol(req_contents + strlen("Content-Length:"), &endptr, base);
        }
    }

    printf("Content-Length: %d\n", content_length);

    if(strstr(req_line, "HTTP/") == NULL)
    {
        send_error(clnt_write);
    }

    // Extract method using strtok_r
    char *saveptr;
    char *token = strtok_r(req_line, " /", &saveptr);
    if(token != NULL)
    {
        strcpy(method, token);
    }
    else
    {
        send_error(clnt_write);
    }

    if(strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0 && strcmp(method, "POST") != 0)
    {
        send_error(clnt_write);
    }

    printf("method 값: %s\n", method);

    // Extract file name using strtok_r
    token = strtok_r(NULL, " /", &saveptr);
    if(token != NULL)
    {
        // 파일 이름을 가져옵니다.
        strcpy(file_name, token);
    }
    else
    {
        // 토큰이 NULL인 경우, 오류 처리를 수행합니다.
        fprintf(stderr, "Failed to extract file name.\n");

        // 또는 다른 오류 처리 방법을 선택하세요.
    }

    printf("File name1: %s\n", file_name);
    // 다음 토큰을 계속해서 가져와서 파일 이름에 추가합니다.
    while((token = strtok_r(NULL, " /", &saveptr)) != NULL)
    {
        if(strcmp(token, "HTTP") == 0)
        {
            break;    // 'http' 토큰을 발견하면 루프를 종료합니다.
        }
        strcat(file_name, "/");
        strcat(file_name, token);    // 다음 토큰을 파일 이름에 추가
    }

    // 파일 이름 출력
    printf("File name: %s\n", file_name);

    // 파일 이름을 기반으로 콘텐츠 타입 결정
    strcpy(ct, content_type(file_name));

    if(strcmp(method, "HEAD") == 0)
    {
        // Send the HTTP response header
        fprintf(clnt_write, "HTTP/1.0 200 OK\r\n");
        fprintf(clnt_write, "Server: Simple HTTP Server\r\n");

        // Calculate and print the content type
        fprintf(clnt_write, "Content-Type: %s\r\n", ct);

        // Print the content length
        fprintf(clnt_write, "Content-Length: %d\r\n", content_length);

        fprintf(clnt_write, "\r\n");    // 빈 줄로 헤더를 끝냅니다.

        fflush(clnt_write);    // 출력 버퍼를 비웁니다.
        fclose(clnt_read);
        fclose(clnt_write);
        return;
    }
    if(strcmp(method, "POST") == 0)
    {
        // Handle POST request
        // Read POST data from clnt_read
        // Process the data if needed

        GDBM_FILE db;
        char      db_file[] = "post.db";
        db                  = gdbm_open(db_file, GDBM, GDBM_WRCREAT | GDBM_READER | GDBM_WRITER, DBM_MODE, NULL);

        if(!db)
        {
            fprintf(stderr, "Failed to open the database\n");
            exit(EXIT_FAILURE);
        }

        //----------------------------------------------------------------
        handle_post_request(clnt_read, content_length, db);
    }

    send_data(clnt_write, ct, file_name);
    fclose(clnt_read);
    fclose(clnt_write);
}

void send_data(FILE *fp, char *ct, char *file_name)
{
    if(fp != NULL)
    {
        char  protocol[] = "HTTP/1.0 200 OK\r\n";
        char  server[]   = "Server: Simple HTTP Server\r\n";
        char  cnt_type[SMALL_BUF];
        char  buf[BUF_SIZE];
        FILE *send_file;

        // Send the HTTP response header
        sprintf(cnt_type, "Content-type:%s\r\n\r\n", ct);
        printf("File Path: %s\n", file_name);

        send_file = fopen(file_name, "re");
        if(send_file == NULL)
        {
            perror("fopen");    // 파일 열기 실패 시 오류 출력

            send_file = fopen("404.html", "re");
            if(send_file == NULL)
            {
                perror("404.html fopen");
                send_error(fp);
                return;
            }
        }

        // header info
        fputs(protocol, fp);
        fputs(server, fp);
        // fputs(cnt_len, fp); // 콘텐츠 길이는 추후 계산하여 할당

        fputs(cnt_type, fp);

        // Send the content of the requested file
        while(fgets(buf, BUF_SIZE, send_file) != NULL)
        {
            fputs(buf, fp);
        }

        // 파일 포인터 닫기
        fclose(send_file);

        // 출력 버퍼 비우기
        fflush(fp);
    }
    else
    {
        return;
    }
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

const char *content_type(const char *file)
{
    const char *result = NULL;

    char extension[SMALL_BUF];
    char file_name[SMALL_BUF];
    strcpy(file_name, file);
    char *saveptr;                                       // saveptr 변수를 정의합니다.
    char *token = strtok_r(file_name, ".", &saveptr);    // 첫 번째 토큰을 얻습니다.
    if(token != NULL)
    {
        strcpy(extension, token);    // 첫 번째 토큰을 확장자로 복사합니다.
    }

    // strtok_r() 함수의 결과를 검사하고 처리합니다.
    token = strtok_r(NULL, ".", &saveptr);    // 두 번째 토큰을 얻습니다.
    if(token != NULL)
    {
        strcpy(extension, token);    // 두 번째 토큰을 확장자로 복사합니다.
    }

    if(strcmp(extension, "jpg") == 0 || strcmp(extension, "jpeg") == 0)
    {
        result = "image/jpeg";
    }
    else if(strcmp(extension, "gif") == 0)
    {
        result = "image/gif";
    }
    else if(strcmp(extension, "ico") == 0)
    {
        result = "image/x-icon";
    }
    else
    {
        result = "text/html";
    }

    return result;
}

void handle_post_request(FILE *clnt_read, int content_length, GDBM_FILE db)
{
    // content_length 출력
    printf("Content Length: %d\n", content_length);

    size_t content_length_s = (size_t)content_length;
    char  *post_data        = (char *)malloc(content_length_s + 1);
    if(post_data == NULL)
    {
        fprintf(stderr, "Failed to allocate memory for post data\n");
        exit(EXIT_FAILURE);
    }

    // 데이터 읽기
    size_t bytes_read = fread(post_data, sizeof(char), content_length_s, clnt_read);
    if(bytes_read != content_length_s)
    {
        fprintf(stderr, "Failed to read data from client\n");
        exit(EXIT_FAILURE);
    }
    post_data[content_length_s] = '\0';    // Null terminate the string
    printf("Value: %s\n", post_data);
    // post_data를 키와 값으로 분리
    char *saveptr;
    char *key_str = strtok_r(post_data, "&", &saveptr);

    char *saveptr_key;

    char *key1 = strtok_r(key_str, "=", &saveptr_key);
    char *key2 = strtok_r(NULL, "=", &saveptr_key);

    char *saveptr_value;

    char *value_str = strtok_r(NULL, "&", &saveptr);
    char *value1    = strtok_r(value_str, "=", &saveptr_value);
    char *value2    = strtok_r(NULL, "=", &saveptr_value);

    printf("%sKey: %s\n", key1, key2);
    printf("%sValue: %s\n", value1, value2);

    // key_str과 value_str에는 각각 "post_data_key"와 "this+is+what%3F"가 저장됩니다.

    // 키와 값 설정
    datum key;
    datum value;
    datum result;
    key.dptr    = key2;
    key.dsize   = (int)strlen(key2);
    value.dptr  = value2;
    value.dsize = (int)strlen(value2);

    //    // 데이터베이스에 저장
    //    if(gdbm_store(db, key, value, GDBM_INSERT) != 0)
    //    {
    //        fprintf(stderr, "Failed to store data in the database: %d\n", gdbm_error(db));
    //        free(post_data);    // Free allocated memory
    //        exit(EXIT_FAILURE);
    //    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waggregate-return"
    result = gdbm_fetch(db, key);
#pragma GCC diagnostic pop
    if(result.dptr == NULL)
    {
        printf("Key not found in the database.\n");

        // 데이터베이스에 저장
        if(gdbm_store(db, key, value, GDBM_INSERT) != 0)
        {
            fprintf(stderr, "Failed to store data in the database: \n");
        }
    }
    else
    {
        printf("dbValue: %s\n", result.dptr);
    }
    free(post_data);    // Free allocated memory
    // 데이터베이스 닫기
    gdbm_close(db);
}