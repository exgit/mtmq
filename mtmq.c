/* Multi-Threaded Memory-based message Queue.
 *
 */


#include "mtmq.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>


/* Clock type to use when waiting on condition variable.
 *
 * Use of CLOCK_MONOTONIC ensures waiting does not hang if
 * host time is set back in time. But it isn't supported on MinGW.
 */
#ifdef _WIN32
#   define MTMQ_CLOCK_TYPE CLOCK_REALTIME
#else
#   define MTMQ_CLOCK_TYPE CLOCK_MONOTONIC
#endif


// Queue element.
typedef struct mtmq_elt {
    int code;
    void *data;
} mtmq_elt_t;


// Queue.
struct mtmq {
    pthread_mutex_t mtx;  // mutex
    pthread_cond_t cond_rd;  // condition variable for readers
    pthread_cond_t cond_wr;  // condition variable for writers
    int num_rd;  // number of currently waiting readers
    int num_wr;  // number of currently waiting writers

    int fin;  // finalized flag

    int num;  // number of elements in queue
    int first;  // index of first element in queue
    int last;  // index of last element in queue + 1

    int size;  // queue max size
    struct mtmq_elt *arr;  // queue elements array
};


// Internal helper function for timeout calculation.
static void calc_abs_timeout(struct timespec *ts, int timeout_ms)
{
    struct timespec t;
    t.tv_nsec = timeout_ms;  // store to wider type
    t.tv_nsec *= 1000000;  // convert milliseconds to nanosecons

    clock_gettime(MTMQ_CLOCK_TYPE, ts);
    ts->tv_nsec += t.tv_nsec;
    ts->tv_sec += ts->tv_nsec / 1000000000;
    ts->tv_nsec = ts->tv_nsec % 1000000000;
}


/* Create queue.
 * In:
 *   size - queue size
 * Out:
 *   NULL - create failed
 *   not NULL - pointer to created queue
 */
mtmq_t *mtmq_create(int size)
{
    mtmq_t *ret;
    int rc;

    size_t mtmq_size = sizeof(mtmq_t);
    mtmq_size += (~mtmq_size + 1) & (sizeof(void*)-1);

    size_t arr_size = sizeof(mtmq_elt_t) * size;

    ret = malloc(mtmq_size + arr_size);
    if (ret == NULL)
        return NULL;

    memset(ret, 0, mtmq_size + arr_size);
    ret->size = size;
    ret->arr = (mtmq_elt_t*)(ret+1);

    pthread_mutex_init(&ret->mtx, NULL);

    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    rc = pthread_condattr_setclock(&ca, MTMQ_CLOCK_TYPE);
    if (rc != 0) {
        pthread_condattr_destroy(&ca);
        pthread_mutex_destroy(&ret->mtx);
        free(ret);
        return NULL;
    }

    pthread_cond_init(&ret->cond_rd, &ca);
    pthread_cond_init(&ret->cond_wr, &ca);
    pthread_condattr_destroy(&ca);

    return ret;
}


/* Destroy queue.
 * In:
 *   q - queue pointer
 * Out:
 *   MTMQ_RC_OK - queue deleted
 *   MTMQ_RC_ERROR - some error occured
 */
int mtmq_destroy(mtmq_t *q)
{
    if (!q)
        return MTMQ_RC_ERROR;

    int rc = pthread_cond_destroy(&q->cond_wr);
    if (rc)
        return MTMQ_RC_ERROR;

    rc = pthread_cond_destroy(&q->cond_rd);
    if (rc)
        return MTMQ_RC_ERROR;

    rc = pthread_mutex_destroy(&q->mtx);
    if (rc)
        return MTMQ_RC_ERROR;

    free(q);
    return MTMQ_RC_OK;
}


/* Push message to queue.
 * In:
 *   q - queue
 *   code - integer code to put to queue
 *   data - pointer to some application data to put to queue
 *   timeout - timeout in milliseconds for operation to complete
 *     if timeout < 0, then wait indefinately for queue to become
 *     available for writing or is finalized.
 * Out:
 *   MTMQ_RC_OK - done
 *   MTMQ_RC_FINALIZED - not done because queue is finalized
 *   MTMQ_RC_TIMEDOUT - not done because of timeout
 *   MTMQ_RC_INTERRUPTED - not done because waiting on condition variable was
 *     interrupted by signal (note that on posix-compliant system this should
 *     never happen)
 *   MTMQ_RC_ERROR - some error occured that requires investigation and debugging.
 */
int mtmq_push(mtmq_t *q, int code, void *data, int timeout)
{
    int ret, rc;

    if (!q)
        return MTMQ_RC_ERROR;

    rc = pthread_mutex_lock(&q->mtx);
    if (rc != 0)
        return MTMQ_RC_ERROR;

    q->num_wr++;
    if (timeout < 0) {
        for (rc=0; !q->fin && q->num==q->size && rc==0; ) {
            rc = pthread_cond_wait(&q->cond_wr, &q->mtx);
        }
    } else {
        struct timespec to;
        calc_abs_timeout(&to, timeout);
        for (rc=0; !q->fin && q->num==q->size && rc==0; ) {
            rc = pthread_cond_timedwait(&q->cond_wr, &q->mtx, &to);
        }
    }
    q->num_wr--;

    if (q->fin)
        ret = MTMQ_RC_FINALIZED;
    else if (q->num < q->size) {
        mtmq_elt_t *e = &q->arr[q->last];
        e->code = code;
        e->data = data;
        q->last = (q->last+1 < q->size) ? (q->last+1) : 0;
        q->num++;
        if (q->num_rd)
            pthread_cond_signal(&q->cond_rd);
        ret = MTMQ_RC_OK;
    } else if (rc == ETIMEDOUT)
        ret = MTMQ_RC_TIMEDOUT;
    else if (rc == EINTR)
        ret = MTMQ_RC_INTERRUPTED;
    else
        ret = MTMQ_RC_ERROR;

    pthread_mutex_unlock(&q->mtx);

    return ret;
}


/* Pop message from queue.
 * In:
 *   q - queue
 *   [out]code - integer code to retrieve from queue
 *   [out]data - pointer to some application data to retrieve from queue
 *   timeout - timeout in milliseconds for operation to complete
 *     if timeout < 0, then wait indefinately for queue to become
 *     available for reading or is finalized.
 * Out:
 *   MTMQ_RC_OK - done
 *   MTMQ_RC_FINALIZED - not done because queue is finalized
 *   MTMQ_RC_TIMEDOUT - not done because of timeout
 *   MTMQ_RC_INTERRUPTED - not done because waiting on condition variable was
 *     interrupted by signal (note that on posix-compliant system this should
 *     never happen)
 *   MTMQ_RC_ERROR - some error occured that requires investigation and debugging.
 * Note:
 *   When queue is finalized consumer(s) will retrieve all available data from
 *   queue before it(they) get MTMQ_RC_FINALIZED return code.
 */
int mtmq_pop(mtmq_t *q, int *code, void **data, int timeout)
{
    int ret, rc;

    if (!q)
        return MTMQ_RC_ERROR;

    rc = pthread_mutex_lock(&q->mtx);
    if (rc != 0)
        return MTMQ_RC_ERROR;

    q->num_rd++;
    if (timeout < 0) {
        for (rc=0; !q->num && !q->fin && rc==0; ) {
            rc = pthread_cond_wait(&q->cond_rd, &q->mtx);
        }
    } else {
        struct timespec to;
        calc_abs_timeout(&to, timeout);
        for (rc=0; !q->num && !q->fin && rc==0; ) {
            rc = pthread_cond_timedwait(&q->cond_rd, &q->mtx, &to);
        }
    }
    q->num_rd--;

    if (q->num) {
        mtmq_elt_t *e = &q->arr[q->first];
        *code = e->code;
        *data = e->data;
        q->first = (q->first+1 < q->size) ? (q->first+1) : 0;
        q->num--;
        if (q->num_wr)
            pthread_cond_signal(&q->cond_wr);
        ret = MTMQ_RC_OK;
    } else if (q->fin)
        ret = MTMQ_RC_FINALIZED;
    else if (rc == ETIMEDOUT)
        ret = MTMQ_RC_TIMEDOUT;
    else if (rc == EINTR)
        ret = MTMQ_RC_INTERRUPTED;
    else
        ret = MTMQ_RC_ERROR;

    pthread_mutex_unlock(&q->mtx);

    return ret;
}


/* Finalize message processing by this queue.
 * In:
 *   q - queue
 * Note:
 *   After finalizing queue caller should make sure all producers and consumers
 *   are not using this queue any more and then destroy queue by calling mtmq_destroy(q).
 */
void mtmq_finalize(mtmq_t *q)
{
    if (!q)
        return;

    pthread_mutex_lock(&q->mtx);
    if (!q->fin) {
        q->fin = 1;
        if (q->num_rd) pthread_cond_broadcast(&q->cond_rd);
        if (q->num_wr) pthread_cond_broadcast(&q->cond_wr);
    }
    pthread_mutex_unlock(&q->mtx);
}


/* Check if queue is finalized.
 * In:
 *   q - queue
 * Out:
 *   0 - queue is not finalized
 *   1 - queue is finalized
 */
int mtmq_is_finalized(mtmq_t *q)
{
    int ret = 1;

    if (q) {
        pthread_mutex_lock(&q->mtx);
        ret = q->fin;
        pthread_mutex_unlock(&q->mtx);
    }

    return ret;
}
