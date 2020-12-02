#define _GNU_SOURCE // needed for RTLD_NEXT

#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define NON_NULL(S) do { if (!S) { fprintf(stderr, "couldn't load symbol: " #S "\n"); abort(); } } while (false)

static int (*next_accept)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
static int (*next_pthread_cond_destroy)(pthread_cond_t *cond);
static int (*next_pthread_cond_init)(pthread_cond_t *restrict cond, const pthread_condattr_t *restrict attr);
static int (*next_pthread_create)(pthread_t *thread, const pthread_attr_t *attr,
        void *(*start_routine)(void*), void *arg);
static int (*next_pthread_join)(pthread_t thread, void **retval);
static ssize_t (*next_recv)(int sockfd, void *buf, size_t len, int flags);
static int (*next_sem_destroy)(sem_t *sem);
static int (*next_sem_init)(sem_t *sem, int pshared, unsigned int value);
static sem_t *(*next_sem_open)(const char *name, int oflag, ...);
static int (*next_sem_post)(sem_t *sem);
static int (*next_sem_timedwait)(sem_t *sem, const struct timespec *abs_timeout);
static int (*next_sem_wait)(sem_t *sem);

/**
 * Records the number of pending wakeups on a semaphore
 * so we don't falsely believe we're idle when we're pending a wakeup.
 * When a thread goes to sleep on a semaphore, it is only counted
 * as idle if it has no pending wakeups.
 */
typedef struct {
    sem_t *sem; // semaphore is guaranteed/required to have a stable address.
    bool named_semaphore; // doesn't count as blocked because it gets external wakeups
    int pending_wakeups;
} SemaphoreInfo;

/**
 * Condition variables are basically impossible to support.
 *
 * This is because there is absolutely no determinism in them. When you say
 * `pthread_cond_signal`, "at least one" sleeping thread will wake up. Furthermore,
 * sleeping threads may wake up for any reason - a signal to another thread, an interrupt,
 * suspend/resume, an earthquake, who even knows. As such, we may end up in a situation
 * where we say that the process is idle, then a few ms later a `pthread_cond_wait`
 * spontaneously decides to return, messing up the process state.
 *
 * Because of this, we reimplement condition variables on top of semaphores, who are nice
 * and predictable, and for whom we already have handling anyways.
 *
 * This works like so: every condition variable initializes two semaphores, which we'll
 * call "IN" and "OUT". When a thread goes to sleep on a condition variable, it increments
 * the number of waiting threads, and `sem_wait`s on OUT.
 * When a thread tries to signal on the condition variable, it is always treated as a
 * broadcast. This is safe, because as said above, condition waiting threads may wake
 * up for basically any reason they want anyways.
 *
 * So when we `pthread_cond_broadcast`, we first copy out IN and OUT and reinitialize them
 * with fresh semaphores.
 * This is so that future waits will only be woken up by future signals.
 * Then we post waiting_threads tokens on OUT and await waiting_threads tokens on IN.
 * This is so that all threads that are currently using the OUT semaphore to sleep
 * finish touching it.
 * Finally, we can now free the two semaphores and return.
 */
typedef struct {
    pthread_cond_t *cond;
    sem_t *in, *out; // malloced because it needs to be stable across reallocs
    int sleeping_threads;
} ConditionInfo;

typedef struct {
    pthread_t id;
    bool sleeping;
    /**
     * Forces the thread to remain marked as idle even if it is "awake".
     * This is so that things like message receive loops can avoid incrementing
     * the idle counter until they've received and dispatched a full message.
     */
    bool forced_idle;
    // non-null when waiting on a semaphore, requires sleeping=true
    sem_t *waiting_semaphore;
} ThreadInfo;

static struct {
    // locks access to state
    pthread_mutex_t mutex;

    int filedes;
    bool locked;
    int times_idle;
    bool verbose;

    size_t sem_info_len;
    SemaphoreInfo *sem_info_ptr;

    size_t cond_info_len;
    ConditionInfo *cond_info_ptr;

    size_t thr_info_len;
    ThreadInfo *thr_info_ptr;
} state = { 0 };

static ThreadInfo *find_thread_info();
static void maybe_lock();
static void maybe_unlock();
static void print_block_map();
static int num_active_threads();

static SemaphoreInfo *libidle_find_sem_info(sem_t *sem)
{
    for (int i = 0; i < state.sem_info_len; i++)
    {
        if (state.sem_info_ptr[i].sem == sem)
        {
            return &state.sem_info_ptr[i];
        }
    }
    return NULL;
}

static ConditionInfo *libidle_find_cond_info(pthread_cond_t *cond)
{
    for (int i = 0; i < state.cond_info_len; i++)
    {
        if (state.cond_info_ptr[i].cond == cond)
        {
            return &state.cond_info_ptr[i];
        }
    }
    return NULL;
}

static ThreadInfo *libidle_find_thr_info(pthread_t id)
{
    for (int i = 0; i < state.thr_info_len; i++)
    {
        if (state.thr_info_ptr[i].id == id)
        {
            return &state.thr_info_ptr[i];
        }
    }
    return NULL;
}

/**
 * Has this thread gone to sleep in a way that will prevent it from waking up on its own?
 */
static bool threadinfo_is_blocked(ThreadInfo *thr_info)
{
    if (thr_info->forced_idle)
    {
        return true;
    }
    if (!thr_info->sleeping)
    {
        return false;
    }
    if (!thr_info->waiting_semaphore)
    {
        return true;
    }
    SemaphoreInfo *sem_info = libidle_find_sem_info(thr_info->waiting_semaphore);
    return sem_info->pending_wakeups > 0 ? false : true;
}

// called when we've gone busy
static void libidle_lock()
{
    assert(!state.locked);
    // printf("lock %i\n", state.filedes);
    flock(state.filedes, LOCK_EX);
    state.locked = true;
}

// called when we've gone idle
static void libidle_unlock()
{
    assert(state.locked);
    // printf("unlock %i\n", state.filedes);
    lseek(state.filedes, 0, SEEK_SET);
    ftruncate(state.filedes, 0);
    dprintf(state.filedes, "%i\n", ++state.times_idle);
    flock(state.filedes, LOCK_UN);
    state.locked = false;
}

static void libidle_register_thread(pthread_t thread)
{
    state.thr_info_ptr = realloc(state.thr_info_ptr, sizeof(ThreadInfo) * ++state.thr_info_len);
    state.thr_info_ptr[state.thr_info_len - 1] = (ThreadInfo) {
        .id = thread,
        .sleeping = false,
        .forced_idle = false,
        .waiting_semaphore = NULL,
    };
}

__attribute__ ((constructor))
void libidle_init()
{
    next_accept = dlsym(RTLD_NEXT, "accept");
    next_pthread_cond_destroy = dlvsym(RTLD_NEXT, "pthread_cond_destroy", "GLIBC_2.3.2");
    next_pthread_cond_init = dlvsym(RTLD_NEXT, "pthread_cond_init", "GLIBC_2.3.2");
    next_pthread_create = dlsym(RTLD_NEXT, "pthread_create");
    next_pthread_join = dlsym(RTLD_NEXT, "pthread_join");
    next_recv = dlsym(RTLD_NEXT, "recv");
    next_sem_destroy = dlsym(RTLD_NEXT, "sem_destroy");
    next_sem_init = dlsym(RTLD_NEXT, "sem_init");
    next_sem_open = dlsym(RTLD_NEXT, "sem_open");
    next_sem_post = dlsym(RTLD_NEXT, "sem_post");
    next_sem_timedwait = dlsym(RTLD_NEXT, "sem_timedwait");
    next_sem_wait = dlsym(RTLD_NEXT, "sem_wait");

    char *statefile = getenv("LIBIDLE_STATEFILE");
    if (!statefile) statefile = ".libidle_state";

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&state.mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    state.filedes = open(statefile, O_RDWR | O_CREAT | O_TRUNC, 0600);
    state.verbose = getenv("LIBIDLE_VERBOSE") ? 1 : 0;
    libidle_register_thread(pthread_self());
    libidle_lock();
}

static void entering_blocked_op()
{
    pthread_mutex_lock(&state.mutex);

    ThreadInfo *thr_info = find_thread_info();
    if (thr_info) thr_info->sleeping = true;
    maybe_unlock();

    pthread_mutex_unlock(&state.mutex);
}

static void left_blocked_op()
{
    pthread_mutex_lock(&state.mutex);

    ThreadInfo *thr_info = find_thread_info();
    if (thr_info) thr_info->sleeping = false;
    maybe_lock();

    pthread_mutex_unlock(&state.mutex);
}

// equivalent to entering a blocked op
void libidle_enable_forced_idle()
{
    pthread_mutex_lock(&state.mutex);

    ThreadInfo *thr_info = find_thread_info();
    assert(thr_info);
    thr_info->forced_idle = true;
    maybe_unlock();

    pthread_mutex_unlock(&state.mutex);
}

// equivalent to leaving a blocked op
void libidle_disable_forced_idle()
{
    pthread_mutex_lock(&state.mutex);

    ThreadInfo *thr_info = find_thread_info();
    assert(thr_info);
    thr_info->forced_idle = false;
    maybe_lock();

    pthread_mutex_unlock(&state.mutex);
}

static void maybe_lock()
{
    /**
     * It is not the case that leaving a blocking op necessarily acquires lock.
     * For instance, the blocking op may be inside a forced_idle pair.
     * In that case, the lock is acquired when we disable forced_idle, bringing
     * the active thread count up.
     */
    if (state.verbose)
    {
        printf("%lu: - block -> ", pthread_self());
        print_block_map();
    }
    if (!state.locked && num_active_threads() > 0)
    {
        if (state.verbose)
        {
            printf("  lock\n");
        }
        libidle_lock();
    }
}

static void maybe_unlock()
{
    if (state.verbose)
    {
        printf("%lu: + block -> ", pthread_self());
        print_block_map();
    }
    if (state.locked && num_active_threads() == 0)
    {
        if (state.verbose)
        {
            printf("  unlock\n");
        }
        libidle_unlock();
    }
}

static ThreadInfo *find_thread_info()
{
    pthread_t self = pthread_self();
    for (int i = 0; i < state.thr_info_len; i++)
    {
        ThreadInfo *thr_info = &state.thr_info_ptr[i];

        if (thr_info->id == self)
        {
            return thr_info;
        }
    }
    return NULL;
}

static void print_block_map()
{
    for (int i = 0; i < state.thr_info_len; i++)
    {
        ThreadInfo *thr_info = &state.thr_info_ptr[i];

        if (i) printf("|");
        printf(threadinfo_is_blocked(thr_info) ? "x" : "-");
    }
    printf("\n");
}

static int num_active_threads()
{
    int active_threads = 0;
    for (int i = 0; i < state.thr_info_len; i++)
    {
        ThreadInfo *thr_info = &state.thr_info_ptr[i];
        active_threads += threadinfo_is_blocked(thr_info) ? 0 : 1;
    }
    return active_threads;
}

//
// function proxies
//
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    NON_NULL(next_accept);
    entering_blocked_op();
    int ret = next_accept(sockfd, addr, addrlen);
    left_blocked_op();
    return ret;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
        void *(*start_routine) (void *), void *arg)
{
    NON_NULL(next_pthread_create);
    int ret = next_pthread_create(thread, attr, start_routine, arg);
    if (ret == 0)
    {
        libidle_register_thread(*thread);
    }
    return ret;
}

int pthread_join(pthread_t thread, void **retval)
{
    NON_NULL(next_pthread_join);
    entering_blocked_op();
    // TODO wakeup signalling on thread destruction
    int ret = next_pthread_join(thread, retval);
    left_blocked_op();
    return ret;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    NON_NULL(next_recv);
    entering_blocked_op();
    ssize_t ret = next_recv(sockfd, buf, len, flags);
    left_blocked_op();
    return ret;
}

sem_t *sem_open(const char *name, int oflag, ...)
{
    NON_NULL(next_sem_open);

    sem_t *ret;
    if (oflag & O_CREAT)
    {
        va_list args;
        va_start(args, oflag);
        mode_t mode = va_arg(args, mode_t);
        unsigned int value = va_arg(args, unsigned int);
        va_end(args);
        ret = next_sem_open(name, oflag, mode, value);
    }
    else
    {
        ret = next_sem_open(name, oflag);
    }
    if (ret == SEM_FAILED) return ret;

    pthread_mutex_lock(&state.mutex);

    // register semaphore in SemaphoreInfo list
    state.sem_info_ptr = realloc(state.sem_info_ptr, sizeof(SemaphoreInfo) * ++state.sem_info_len);
    state.sem_info_ptr[state.sem_info_len - 1] = (SemaphoreInfo) { .sem = ret, .named_semaphore = true };

    pthread_mutex_unlock(&state.mutex);

    return 0;
}

int sem_init(sem_t *sem, int pshared, unsigned int value)
{
    NON_NULL(next_sem_init);

    /**
     * see sem_init docs:
     *
     * > If  pshared  has  the  value  0,  then the semaphore is shared between the
     * > threads of a process, and should be located at some address that is visible
     * > to all threads (e.g., a global variable, or a variable allocated
     * > dynamically on the heap).
     */
    int ret = next_sem_init(sem, pshared, value);
    if (ret != 0) return ret;

    pthread_mutex_lock(&state.mutex);

    // register semaphore in SemaphoreInfo list
    state.sem_info_ptr = realloc(state.sem_info_ptr, sizeof(SemaphoreInfo) * ++state.sem_info_len);
    state.sem_info_ptr[state.sem_info_len - 1] = (SemaphoreInfo) { .sem = sem, .pending_wakeups = value };

    pthread_mutex_unlock(&state.mutex);

    return 0;
}

int sem_destroy(sem_t *sem)
{
    NON_NULL(next_sem_destroy);

    pthread_mutex_lock(&state.mutex);

    // TODO iterate backwards- faster? more recently created sems are likely to be at the back
    for (int i = 0; i < state.sem_info_len; i++)
    {
        SemaphoreInfo *sem_info = &state.sem_info_ptr[i];
        if (sem_info->sem == sem)
        {
            // swap with last entry and shrink
            *sem_info = state.sem_info_ptr[state.sem_info_len - 1];
            state.sem_info_ptr = realloc(state.sem_info_ptr, sizeof(SemaphoreInfo) * --state.sem_info_len);
            break;
        }
    }
    // should assert we actually freed something rn... meh

    pthread_mutex_unlock(&state.mutex);

    return next_sem_destroy(sem);
}

int sem_post(sem_t *sem)
{
    NON_NULL(next_sem_post);

    pthread_mutex_lock(&state.mutex);

    SemaphoreInfo *sem_info = libidle_find_sem_info(sem);

    assert(sem_info);
    sem_info->pending_wakeups++;

    pthread_mutex_unlock(&state.mutex);

    return next_sem_post(sem);
}

static int libidle_sem_wait(bool timedwait, sem_t *sem, const struct timespec *abs_timeout);

/**
 * When we signal a semaphore, we don't know which sleeping semaphore will wake up.
 * Because of this, we must track additionally the number of *pending* wakeups.
 * This is incremented in sem_post.
 * Conversely, sem_wait decrements.
 */
int sem_wait(sem_t *sem)
{
    return libidle_sem_wait(false, sem, NULL);
}

int sem_timedwait(sem_t *sem, const struct timespec *abs_timeout)
{
    return libidle_sem_wait(true, sem, abs_timeout);
}

static int libidle_sem_wait(bool timedwait, sem_t *sem, const struct timespec *abs_timeout)
{
    NON_NULL(next_sem_wait);
    NON_NULL(next_sem_timedwait);

    pthread_mutex_lock(&state.mutex);

    SemaphoreInfo *sem_info = libidle_find_sem_info(sem);
    assert(sem_info);

    ThreadInfo *thr_info = libidle_find_thr_info(pthread_self());
    assert(thr_info);

    if (!sem_info->named_semaphore)
    {
        thr_info->waiting_semaphore = sem;
    }

    pthread_mutex_unlock(&state.mutex);

    if (!sem_info->named_semaphore)
    {
        entering_blocked_op();
    }

    int ret;
    if (timedwait)
    {
        ret = next_sem_timedwait(sem, abs_timeout);
    }
    else
    {
        ret = next_sem_wait(sem);
    }

    /**
     * order matters here!
     * - unblock the op
     * - unlink the semaphore
     * - then decrement the wakeups.
     * the point is that we must enter a known state of wakefulness before we
     * untrack the semaphore. otherwise, libidle may miss the thread having woken.
     */
    if (!sem_info->named_semaphore)
    {
        pthread_mutex_lock(&state.mutex);

        left_blocked_op();

        thr_info->waiting_semaphore = NULL;
        sem_info->pending_wakeups--;

        pthread_mutex_unlock(&state.mutex);
    }

    return ret;
}

int pthread_cond_init_232(pthread_cond_t *restrict cond, const pthread_condattr_t *restrict attr)
{
    pthread_mutex_lock(&state.mutex);
    // printf("  register %p\n", cond);

    // register condition in ConditionInfo list
    state.cond_info_ptr = realloc(state.cond_info_ptr, sizeof(ConditionInfo) * ++state.cond_info_len);
    ConditionInfo *info = &state.cond_info_ptr[state.cond_info_len - 1];

    *info = (ConditionInfo) {
        .cond = cond,
        .in = malloc(sizeof(sem_t)),
        .out = malloc(sizeof(sem_t)),
        .sleeping_threads = 0,
    };
    // our own function - not next_!
    sem_init(info->in, 0, 0);
    sem_init(info->out, 0, 0);

    pthread_mutex_unlock(&state.mutex);

    return 0;
}

int pthread_cond_destroy_232(pthread_cond_t *cond)
{
    NON_NULL(next_pthread_cond_destroy);

    pthread_mutex_lock(&state.mutex);

    // TODO iterate backwards- faster? more recently created sems are likely to be at the back
    for (int i = 0; i < state.cond_info_len; i++)
    {
        ConditionInfo *cond_info = &state.cond_info_ptr[i];
        if (cond_info->cond == cond)
        {
            // pthread_cond_destroy undefined if we're still waiting on this condition
            assert(cond_info->sleeping_threads == 0);

            sem_destroy(cond_info->in);
            sem_destroy(cond_info->out);
            free(cond_info->in);
            free(cond_info->out);

            // swap with last entry and shrink
            *cond_info = state.cond_info_ptr[state.cond_info_len - 1];
            state.cond_info_ptr = realloc(state.cond_info_ptr, sizeof(ConditionInfo) * --state.cond_info_len);
            break;
        }
    }
    // should assert we actually freed something rn... meh

    pthread_mutex_unlock(&state.mutex);

    return next_pthread_cond_destroy(cond);
}

int pthread_cond_wait_232(pthread_cond_t *restrict cond, pthread_mutex_t *restrict mutex)
{
    pthread_mutex_lock(&state.mutex);

    // mutex is locked here per condition semantics. however, we can safely release it at this
    // point because we hold state.mutex anyway.
    pthread_mutex_unlock(mutex);

    ConditionInfo *cond_info = libidle_find_cond_info(cond);
    // assert(cond_info);

    // printf("> sleep on %p: sem %p\n", cond, cond_info->in);

    cond_info->sleeping_threads++;
    sem_t *in = cond_info->in, *out = cond_info->out;

    pthread_mutex_unlock(&state.mutex); // all state modifications are done.

    sem_wait(in);
    // printf("cond waiter woke up.\n");
    sem_post(out);

    // grab the mutex back
    pthread_mutex_lock(mutex);

    return 0;
}

int pthread_cond_timedwait_232(pthread_cond_t *restrict cond, pthread_mutex_t *restrict mutex,
        const struct timespec *restrict abstime)
{
    // TODO sem_timedwait
    return pthread_cond_wait_232(cond, mutex);
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    // allowed under condition semantics!
    return pthread_cond_broadcast(cond);
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    pthread_mutex_lock(&state.mutex);

    ConditionInfo *cond_info = libidle_find_cond_info(cond);
    ThreadInfo *thr_info = libidle_find_thr_info(pthread_self());

    assert(thr_info);
    assert(cond_info);

    sem_t *in = cond_info->in, *out = cond_info->out;
    int were_sleeping = cond_info->sleeping_threads;
    // printf("> broadcast to %i (%p)\n", were_sleeping, cond);

    // reinit cond_info - create a new "cond_wait/cond_signal group".
    cond_info->in = malloc(sizeof(sem_t));
    cond_info->out = malloc(sizeof(sem_t));
    sem_init(cond_info->in, 0, 0);
    sem_init(cond_info->out, 0, 0);
    cond_info->sleeping_threads = 0;

    pthread_mutex_unlock(&state.mutex); // done with state mutation

    // printf("> distribute tokens\n");
    for (int i = 0; i < were_sleeping; i++)
    {
        // printf("post sem %p\n", in);
        sem_post(in);
    }

    // printf("> collect tokens\n");
    for (int i = 0; i < were_sleeping; i++)
    {
        sem_wait(out);
    }
    // printf("> tokens collected\n");
    // because we've waited for out, we can now clean up the semaphores.
    sem_destroy(in);
    sem_destroy(out);
    free(in);
    free(out);

    return 0;
}

// see http://blog.fesnel.com/blog/2009/08/25/preloading-with-multiple-symbol-versions/
// see https://code.woboq.org/userspace/glibc/nptl/pthread_cond_init.c.html
__asm__(".symver pthread_cond_destroy_232, pthread_cond_destroy@@GLIBC_2.3.2");
__asm__(".symver pthread_cond_init_232, pthread_cond_init@@GLIBC_2.3.2");
__asm__(".symver pthread_cond_timedwait_232, pthread_cond_timedwait@@GLIBC_2.3.2");
__asm__(".symver pthread_cond_wait_232, pthread_cond_wait@@GLIBC_2.3.2");
