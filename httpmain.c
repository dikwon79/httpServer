#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
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


#define THREAD_POOL_SIZE 4 // 스레드 풀 크기
#define TASK_QUEUE_SIZE 100 // 작업 큐 크기
#define BUF_SIZE 9000
#define SMALL_BUF 1024


// 작업 구조체 정의
typedef struct {
    void (*function)(void*); // 작업 함수 포인터
    void *argument; // 작업 인자
    int completed; // 작업 완료 여부 플래그
} Task;

// 스레드 풀 구조체 정의
typedef struct {
    pthread_t threads[THREAD_POOL_SIZE]; // 스레드 배열
    Task task_queue[TASK_QUEUE_SIZE]; // 작업 큐
    int queue_front; // 큐의 맨 앞 인덱스
    int queue_rear; // 큐의 맨 뒤 인덱스
    pthread_mutex_t queue_mutex; // 큐에 대한 뮤텍스
    pthread_cond_t queue_not_empty; // 작업이 들어올 때까지 대기하는 조건 변수
    pthread_cond_t queue_not_full; // 작업 큐가 가득 차면 대기하는 조건 변수
    int shutdown; // 스레드 풀 종료 여부
    int active_tasks; // 실행 중인 작업 수
    pthread_mutex_t active_tasks_mutex; // 실행 중인 작업 수에 대한 뮤텍스
    pthread_cond_t all_tasks_completed; // 모든 작업이 완료될 때까지 대기하는 조건 변수
} ThreadPool;

void  error_handling(char *message);
void *request_handler(void *arg);
void  send_error(FILE *fp);
void  send_data(FILE *fp, char *ct, char *file_name);
char *content_type(char *file);
void  handle_post_request(FILE *clnt_read, int content_length, DBM *db);
void *thread_function(void *arg);
void thread_pool_init(ThreadPool *pool);
void thread_pool_add_task(ThreadPool *pool, void (*function)(void *), void *argument);
void thread_pool_wait_all_tasks_completed(ThreadPool *pool);
void thread_pool_shutdown(ThreadPool *pool);
void test_task_function(void *arg);
void *thread_function(void *arg);

// Your code follows...


// 스레드 함수
void *thread_function(void *arg) {
    ThreadPool *pool = (ThreadPool *)arg;
    Task task;

    while (1) {
        pthread_mutex_lock(&(pool->queue_mutex));

        // 작업 큐가 비어있을 때까지 대기
        while ((pool->queue_front == pool->queue_rear) && !(pool->shutdown)) {
            pthread_cond_wait(&(pool->queue_not_empty), &(pool->queue_mutex));
        }

        // 스레드 풀 종료 조건 확인
        if (pool->shutdown) {
            pthread_mutex_unlock(&(pool->queue_mutex));
            pthread_exit(NULL);
        }

        // 작업 큐에서 작업 가져오기
        task = pool->task_queue[pool->queue_front];
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
        if (pool->active_tasks == 0) {
            pthread_cond_signal(&(pool->all_tasks_completed));
        }
        pthread_mutex_unlock(&(pool->active_tasks_mutex));
    }

    pthread_exit(NULL);
}

// 스레드 풀 초기화 함수
void thread_pool_init(ThreadPool *pool) {
    int i;

    // 큐 인덱스 초기화
    pool->queue_front = 0;
    pool->queue_rear = 0;

    // 뮤텍스 초기화
    pthread_mutex_init(&(pool->queue_mutex), NULL);
    pthread_cond_init(&(pool->queue_not_empty), NULL);
    pthread_cond_init(&(pool->queue_not_full), NULL);
    pthread_mutex_init(&(pool->active_tasks_mutex), NULL);
    pthread_cond_init(&(pool->all_tasks_completed), NULL);

    // 스레드 풀 생성
    for (i = 0; i < THREAD_POOL_SIZE; ++i) {
        pthread_create(&(pool->threads[i]), NULL, thread_function, (void *)pool);
    }
}

// 작업 추가 함수
void thread_pool_add_task(ThreadPool *pool, void (*function)(void *), void *argument) {
    pthread_mutex_lock(&(pool->queue_mutex));

    // 작업 큐가 가득 찰 때까지 대기
    while (((pool->queue_rear + 1) % TASK_QUEUE_SIZE == pool->queue_front)) {
        pthread_cond_wait(&(pool->queue_not_full), &(pool->queue_mutex));
    }

    // 작업 추가
    pool->task_queue[pool->queue_rear].function = function;
    pool->task_queue[pool->queue_rear].argument = argument;
    pool->task_queue[pool->queue_rear].completed = 0; // 작업이 아직 완료되지 않았음을 표시
    pool->queue_rear = (pool->queue_rear + 1) % TASK_QUEUE_SIZE;
    pool->active_tasks++; // 실행 중인 작업 수 증가

    // 작업이 들어왔음을 통지
    pthread_cond_signal(&(pool->queue_not_empty));
    pthread_mutex_unlock(&(pool->queue_mutex));
}

// 모든 작업이 완료될 때까지 대기
void thread_pool_wait_all_tasks_completed(ThreadPool *pool) {
    pthread_mutex_lock(&(pool->active_tasks_mutex));
    while (pool->active_tasks > 0) {
        pthread_cond_wait(&(pool->all_tasks_completed), &(pool->active_tasks_mutex));
    }
    pthread_mutex_unlock(&(pool->active_tasks_mutex));
}

// 스레드 풀 종료 함수
void thread_pool_shutdown(ThreadPool *pool) {
    int i;

    // 스레드 풀 종료 플래그 설정
    pool->shutdown = 1;

    // 작업이 들어왔음을 통지
    pthread_cond_broadcast(&(pool->queue_not_empty));

    // 스레드 조인
    for (i = 0; i < THREAD_POOL_SIZE; ++i) {
        pthread_join(pool->threads[i], NULL);
    }

    // 뮤텍스 및 조건 변수 해제
    pthread_mutex_destroy(&(pool->queue_mutex));
    pthread_cond_destroy(&(pool->queue_not_empty));
    pthread_cond_destroy(&(pool->queue_not_full));
    pthread_mutex_destroy(&(pool->active_tasks_mutex));
    pthread_cond_destroy(&(pool->all_tasks_completed));
}


// 테스트용 작업 함수
void test_task_function(void *arg) {
    int *num = (int *)arg;
    printf("Task with argument: %d\n", *num);
}

int main(int argc, char *argv[]){

    int                serv_sock, clnt_sock;
    struct sockaddr_in serv_adr, clnt_adr;
    int                clnt_adr_size;
    char               buf[BUF_SIZE];
    ThreadPool pool;

    // 스레드 풀 초기화
    thread_pool_init(&pool);


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

        thread_pool_add_task(&pool, (void *)request_handler, &clnt_sock);

    }
    // 모든 작업이 완료될 때까지 대기
    thread_pool_wait_all_tasks_completed(&pool);

    // 스레드 풀 종료
    thread_pool_shutdown(&pool);


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
    return NULL;


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
        printf("Value: %s\n", value.dptr);
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
    //  fclose(send_file);
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
