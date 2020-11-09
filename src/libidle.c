#define _GNU_SOURCE // needed for RTLD_NEXT

#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int (*next_accept)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
static ssize_t (*next_recv)(int sockfd, void *buf, size_t len, int flags);
static int (*next_sem_wait)(sem_t *sem);
static int (*next_sem_timedwait)(sem_t *sem, const struct timespec *abs_timeout);

static int state_filedes;
static int times_idle = 0;
static int active_threads_count = 1;
static pthread_mutex_t mutex;

// called when we've gone busy
static void libidle_lock()
{
    flock(state_filedes, LOCK_EX);
}

// called when we've gone idle
static void libidle_unlock()
{
    printf("unlock %i\n", state_filedes);
    lseek(state_filedes, 0, SEEK_SET);
    ftruncate(state_filedes, 0);
    dprintf(state_filedes, "%i\n", ++times_idle);
    flock(state_filedes, LOCK_UN);
}

static void *safe_dlsym_next(const char *symbol)
{
    void *ret = dlsym(RTLD_NEXT, symbol);
    assert(ret);
    return ret;
}

__attribute__ ((constructor))
void libidle_init()
{
    next_accept = safe_dlsym_next("accept");
    next_recv = safe_dlsym_next("recv");
    next_sem_wait = safe_dlsym_next("sem_wait");
    next_sem_timedwait = safe_dlsym_next("sem_timedwait");

    char *statefile = getenv("LIBIDLE_STATEFILE");
    if (!statefile) statefile = ".libidle_state";

    pthread_mutex_init(&mutex, NULL);

    state_filedes = open(statefile, O_RDWR | O_CREAT | O_TRUNC, 0600);
    libidle_lock();
}

static void libidle_entering_blocked_op()
{
    pthread_mutex_lock(&mutex);
    if (--active_threads_count == 0)
    {
        libidle_unlock();
    }
    printf("+ block -> %i\n", active_threads_count);
    pthread_mutex_unlock(&mutex);
}

static void libidle_left_blocked_op()
{
    pthread_mutex_lock(&mutex);
    if (active_threads_count++ == 0)
    {
        libidle_lock();
    }
    printf("- block -> %i\n", active_threads_count);
    pthread_mutex_unlock(&mutex);
}

//
// function proxies
//
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    libidle_entering_blocked_op();
    int ret = next_accept(sockfd, addr, addrlen);
    libidle_left_blocked_op();
    return ret;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    libidle_entering_blocked_op();
    ssize_t ret = next_recv(sockfd, buf, len, flags);
    libidle_left_blocked_op();
    return ret;
}

int sem_wait(sem_t *sem)
{
    /**
     * TODO
     * When we signal a semaphore, we don't know which sleeping semaphore will wake up.
     * Because of this, we must track additionally the number of *pending* wakeups.
     * This should be done in sem_post.
     * Conversely, we must decrement it here after the left_blocked_op.
     * Figure out a test for this.
     */
    libidle_entering_blocked_op();
    int ret = next_sem_wait(sem);
    libidle_left_blocked_op();
    return ret;
}

int sem_timedwait(sem_t *sem, const struct timespec *abs_timeout)
{
    libidle_entering_blocked_op();
    int ret = next_sem_timedwait(sem, abs_timeout);
    libidle_left_blocked_op();
    return ret;
}
