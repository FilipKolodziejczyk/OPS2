#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>

#define MIN_CHAR 'a'
#define MAX_CHAR 'j'
#define READ_END 0
#define WRITE_END 1
#define MILI_TO_NANO 1000000
#define SLEEP_TIME_MS (500 * MILI_TO_NANO)
#define MSG_BUF_SIZE PIPE_BUF
#define RESULT_BUF_SIZE (sizeof(pid_t) + sizeof(int))

#define ERR(source) (perror(source),                                 \
                     fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), \
                     exit(EXIT_FAILURE))

#define OPEN_PIPE(fds)   \
    if (pipe(fds) == -1) \
    ERR("open")
#define CLOSE_PIPE(fd)   \
    if (close(fd) == -1) \
    ERR("close")
#define BREAK_IF_PENDING(pending_signals, sigNo)                \
    (if (sigpending(&pending_signals) == -1) ERR("sigpending"), \
     if (sigismember(&pending_signals, sigNo)) break)

void usage(char *name);
bool is_string_valid(char *str);
void child_work(int from_parent_pipe, int to_parent_pipe, int from_other_child_pipe, int to_other_child_pipe);
void parent_work(int to_child1_pipe, int from_child1_pipe, int from_child2_pipe, char *starting_string);
char random_char();
ssize_t remove_char(char *str, ssize_t len, char c);
void print_status(int pid, char c, char *msg, int msg_size);
void set_handler(void (*f)(int), int sigNo);
void set_mask(int sigNo);
bool is_sig_pending(int sigNo);

int main(int argc, char **argv)
{
    set_handler(SIG_IGN, SIGINT);

    if (argc != 2 || !is_string_valid(argv[1]) || strlen(argv[1]) > MSG_BUF_SIZE)
        usage(argv[0]);

    int parent_child1_pipe[2];
    int child1_parent_pipe[2];
    int child1_child2_pipe[2];
    int child2_child1_pipe[2];

    OPEN_PIPE(parent_child1_pipe);
    OPEN_PIPE(child1_parent_pipe);
    OPEN_PIPE(child1_child2_pipe);
    OPEN_PIPE(child2_child1_pipe);

    pid_t pid;
    switch (pid = fork())
    {
    case -1:
        ERR("fork");

    case 0:
        set_mask(SIGINT);
        CLOSE_PIPE(parent_child1_pipe[WRITE_END]);
        CLOSE_PIPE(child1_parent_pipe[READ_END]);
        CLOSE_PIPE(child1_child2_pipe[READ_END]);
        CLOSE_PIPE(child2_child1_pipe[WRITE_END]);

        child_work(parent_child1_pipe[READ_END], child1_parent_pipe[WRITE_END], child2_child1_pipe[READ_END], child1_child2_pipe[WRITE_END]);

        CLOSE_PIPE(parent_child1_pipe[READ_END]);
        CLOSE_PIPE(child1_parent_pipe[WRITE_END]);
        CLOSE_PIPE(child2_child1_pipe[READ_END]);
        CLOSE_PIPE(child1_child2_pipe[WRITE_END]);
        return EXIT_SUCCESS;

    default:
        CLOSE_PIPE(parent_child1_pipe[READ_END]);
        CLOSE_PIPE(child1_parent_pipe[WRITE_END]);
        CLOSE_PIPE(child2_child1_pipe[READ_END]);
        CLOSE_PIPE(child1_child2_pipe[WRITE_END]);
        break;
    }

    int child2_parent_pipe[2];
    OPEN_PIPE(child2_parent_pipe);

    switch (pid = fork())
    {
    case -1:
        ERR("fork");

    case 0:
        set_mask(SIGINT);
        CLOSE_PIPE(parent_child1_pipe[WRITE_END]);
        CLOSE_PIPE(child1_parent_pipe[READ_END]);
        CLOSE_PIPE(child2_parent_pipe[READ_END]);

        child_work(0, child2_parent_pipe[WRITE_END], child1_child2_pipe[READ_END], child2_child1_pipe[WRITE_END]);

        CLOSE_PIPE(child2_parent_pipe[WRITE_END]);
        CLOSE_PIPE(child1_child2_pipe[READ_END]);
        CLOSE_PIPE(child2_child1_pipe[WRITE_END]);
        return EXIT_SUCCESS;

    default:
        CLOSE_PIPE(child2_parent_pipe[WRITE_END]);
        CLOSE_PIPE(child1_child2_pipe[READ_END]);
        CLOSE_PIPE(child2_child1_pipe[WRITE_END]);
        break;
    }

    parent_work(parent_child1_pipe[WRITE_END], child1_parent_pipe[READ_END], child2_parent_pipe[READ_END], argv[1]);

    CLOSE_PIPE(parent_child1_pipe[WRITE_END]);
    CLOSE_PIPE(child1_parent_pipe[READ_END]);
    CLOSE_PIPE(child2_parent_pipe[READ_END]);
    return EXIT_SUCCESS;
}

void parent_work(int to_child1_pipe, int from_child1_pipe, int from_child2_pipe, char *starting_string)
{
    if (TEMP_FAILURE_RETRY(write(to_child1_pipe, starting_string, strlen(starting_string))) != strlen(starting_string))
        ERR("write");

    char result1[RESULT_BUF_SIZE];
    char result2[RESULT_BUF_SIZE];
    if (TEMP_FAILURE_RETRY(read(from_child1_pipe, &result1, sizeof result1)) != sizeof result1)
        ERR("read");
    if (TEMP_FAILURE_RETRY(read(from_child2_pipe, &result2, sizeof result2)) != sizeof result2)
        ERR("read");

    pid_t pid1 = *(pid_t *)result1;
    pid_t pid2 = *(pid_t *)result2;
    int counter1 = *(int *)(result1 + sizeof(pid_t));
    int counter2 = *(int *)(result2 + sizeof(pid_t));

    if (counter1 > counter2)
        printf("Process %d won with the counter %d\n", pid1, counter1);
    else if (counter1 < counter2)
        printf("Process %d won with the counter %d\n", pid2, counter2);
    else
        printf("Processes %d and %d tied with the counter %d\n", pid1, pid2, counter1);
}

void child_work(int from_parent_pipe, int to_parent_pipe, int from_other_child_pipe, int to_other_child_pipe)
{
    pid_t pid = getpid();
    char msg[MSG_BUF_SIZE];
    ssize_t msg_len;
    int counter = 0;
    const struct timespec sleep_time = {.tv_sec = 0, .tv_nsec = SLEEP_TIME_MS};

    srand(time(NULL) * pid);
    char to_remove = random_char();

    sigset_t pending_signals;
    if (sigemptyset(&pending_signals) == -1)
        ERR("sigemptyset");

    int target_pipe = from_parent_pipe != 0 ? from_parent_pipe : from_other_child_pipe;

    for (;;)
    {
        if ((msg_len = TEMP_FAILURE_RETRY(read(target_pipe, msg, sizeof msg))) == -1)
            ERR("read");

        if (target_pipe == from_parent_pipe)
            target_pipe = from_other_child_pipe;

        if (msg_len == 0)
            break;

        if (sigpending(&pending_signals) == -1)
            ERR("sigpending");
        if (sigismember(&pending_signals, SIGINT))
            break;

        to_remove = random_char();
        int removed_chars = remove_char(msg, msg_len, to_remove);
        counter += removed_chars;
        print_status(pid, to_remove, msg, msg_len - removed_chars);
        nanosleep(&sleep_time, NULL);

        if (msg_len - removed_chars == 0)
            break;

        if (TEMP_FAILURE_RETRY(write(to_other_child_pipe, msg, msg_len - removed_chars)) != msg_len - removed_chars)
            ERR("write");
    }

    char result[RESULT_BUF_SIZE];
    *((pid_t *)result) = pid;
    *((int *)(result + sizeof(pid_t))) = counter;
    if (write(to_parent_pipe, result, sizeof result) != sizeof result)
        ERR("write");
}

void print_status(int pid, char c, char *msg, int msg_size)
{
    msg[msg_size] = '\0';
    if (printf("PID %d %c %s\n", pid, c, msg) == -1)
        ERR("printf");
}

char random_char()
{
    return MIN_CHAR + rand() % (MAX_CHAR - MIN_CHAR + 1);
}

ssize_t remove_char(char *str, ssize_t len, char c)
{
    ssize_t counter = 0;
    for (int i = 0, j = 0; i < len; i++)
    {
        if (str[i] != c)
            str[j++] = str[i];
        else
            counter++;
    }

    return counter;
}

void set_handler(void (*f)(int), int sigNo)
{
    struct sigaction act;
    act.sa_handler = f;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    if (sigaction(sigNo, &act, NULL) == -1)
        ERR("sigaction");
}

void set_mask(int sigNo)
{
    sigset_t mask;
    if (sigemptyset(&mask) == -1)
        ERR("sigemptyset");
    if (sigaddset(&mask, sigNo) == -1)
        ERR("sigaddset");
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
        ERR("sigprocmask");
}

void usage(char *name)
{
    fprintf(stderr, "USAGE: %s string[%c-%c][<= %d]\n", name, MIN_CHAR, MAX_CHAR, MSG_BUF_SIZE);
    exit(EXIT_FAILURE);
}

bool is_string_valid(char *str)
{
    while (*str != '\0')
    {
        if (*str < MIN_CHAR || *str > MAX_CHAR)
            return false;
        str++;
    }

    return true;
}