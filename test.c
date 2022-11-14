#include "mtmq.h"

#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>


static mtmq_t *queue;


static volatile int s_break;
void sig_handler(int n) {
    s_break = 1;
    return;
}


static void *thread_wr(void *data);
static void *thread_rd(void *data);


int main()
{
#ifdef _WIN32
    signal(SIGINT, sig_handler);
#else
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = sig_handler;
    sigaction(SIGINT, &sa, NULL);
#endif
    printf("Press Ctrl-C to exit.\n");

    queue = mtmq_create(5);
    if (!queue) {
        printf("mtmq_create() error\n");
        return 1;
    }

    pthread_t tid_wr1;
    pthread_t tid_rd1;
    pthread_create(&tid_wr1, NULL, thread_wr, NULL);
    pthread_create(&tid_rd1, NULL, thread_rd, NULL);

    while (!mtmq_is_finalized(queue)) {
        sleep(1);
        if (s_break) {
            printf("Got SIGINT signal.\n");
            mtmq_finalize(queue);
            break;
        }
    }

    pthread_join(tid_wr1, NULL);
    pthread_join(tid_rd1, NULL);

    int rc = mtmq_destroy(queue);
    if (rc != MTMQ_RC_OK) {
        printf("mtmq_destroy() error\n");
        return 1;
    }

    return 0;
}


static void *thread_wr(void *data)
{
    (void)data;

    for (int i=0; i<16; i++) {
        int rc = mtmq_push(queue, i, NULL, -1);
        if (rc == MTMQ_RC_FINALIZED) {
            printf("Producer: queue is finalized.\n");
            break;
        } else if (rc != MTMQ_RC_OK) {
            printf("Error in mtmq_push().\n");
            break;
        }
    }

    mtmq_finalize(queue);
    printf("Producer exiting.\n");

    return NULL;
}


static void *thread_rd(void *data)
{
    int i;
    void *payload;

    (void)data;

    for (;;) {
        sleep(1);

        int rc = mtmq_pop(queue, &i, &payload, 250);
        if (rc == MTMQ_RC_FINALIZED) {
            printf("Consumer: queue is finalized.\n");
            break;
        }
        if (rc == MTMQ_RC_TIMEDOUT) {
            printf("Consumer: timeout reading from queue.\n");
            continue;
        }
        if (rc != MTMQ_RC_OK) {
            printf("Error in mtmq_pop().\n");
            break;
        }

        printf("%d\n", i);
    }

    printf("Consumer exiting.\n");

    return NULL;
}
