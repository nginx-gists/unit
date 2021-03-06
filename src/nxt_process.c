
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_main_process.h>


static void nxt_process_start(nxt_task_t *task, nxt_process_t *process);
static nxt_int_t nxt_user_groups_get(nxt_task_t *task, nxt_user_cred_t *uc);


/* A cached process pid. */
nxt_pid_t  nxt_pid;

/* An original parent process pid. */
nxt_pid_t  nxt_ppid;


nxt_pid_t
nxt_process_create(nxt_task_t *task, nxt_process_t *process)
{
    nxt_pid_t      pid;
    nxt_process_t  *p;
    nxt_runtime_t  *rt;

    rt = task->thread->runtime;

    pid = fork();

    switch (pid) {

    case -1:
        nxt_log(task, NXT_LOG_CRIT, "fork() failed while creating \"%s\" %E",
                process->init->name, nxt_errno);
        break;

    case 0:
        /* A child. */
        nxt_pid = getpid();

        /* Clean inherited cached thread tid. */
        task->thread->tid = 0;

        process->pid = nxt_pid;

        rt->types = 0;

        nxt_port_reset_next_id();

        nxt_event_engine_thread_adopt(task->thread->engine);

        /* Remove not ready processes */
        nxt_runtime_process_each(rt, p) {

            if (!p->ready) {
                nxt_debug(task, "remove not ready process %PI", p->pid);

                nxt_runtime_process_remove(rt, p);

            } else {
                nxt_port_mmaps_destroy(p->incoming, 0);
                nxt_port_mmaps_destroy(p->outgoing, 0);
            }

        } nxt_runtime_process_loop;

        nxt_runtime_process_add(rt, process);

        nxt_process_start(task, process);

        process->ready = 1;

        break;

    default:
        /* A parent. */
        nxt_debug(task, "fork(\"%s\"): %PI", process->init->name, pid);

        process->pid = pid;

        nxt_runtime_process_add(rt, process);

        break;
    }

    return pid;
}


static void
nxt_process_start(nxt_task_t *task, nxt_process_t *process)
{
    nxt_int_t                    ret;
    nxt_port_t                   *port, *main_port;
    nxt_thread_t                 *thread;
    nxt_runtime_t                *rt;
    nxt_process_init_t           *init;
    nxt_event_engine_t           *engine;
    const nxt_event_interface_t  *interface;

    init = process->init;

    nxt_log(task, NXT_LOG_INFO, "%s started", init->name);

    nxt_process_title(task, "unit: %s", init->name);

    thread = task->thread;

    nxt_random_init(&thread->random);

    if (init->user_cred != NULL && getuid() == 0) {
        /* Super-user. */

        ret = nxt_user_cred_set(task, init->user_cred);
        if (ret != NXT_OK) {
            goto fail;
        }
    }

    rt = thread->runtime;

    rt->types |= (1U << init->type);

    engine = thread->engine;

    /* Update inherited main process event engine and signals processing. */
    engine->signals->sigev = init->signals;

    interface = nxt_service_get(rt->services, "engine", rt->engine);
    if (nxt_slow_path(interface == NULL)) {
        goto fail;
    }

    if (nxt_event_engine_change(engine, interface, rt->batch) != NXT_OK) {
        goto fail;
    }

    ret = nxt_runtime_thread_pool_create(thread, rt, rt->auxiliary_threads,
                                         60000 * 1000000LL);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto fail;
    }

    main_port = rt->port_by_type[NXT_PROCESS_MAIN];

    nxt_port_read_close(main_port);
    nxt_port_write_enable(task, main_port);

    port = nxt_process_port_first(process);

    nxt_port_write_close(port);

    ret = init->start(task, init->data);

    if (nxt_slow_path(ret != NXT_OK)) {
        goto fail;
    }

    nxt_port_enable(task, port, init->port_handlers);

    ret = nxt_port_socket_write(task, main_port, NXT_PORT_MSG_READY,
                                -1, init->stream, 0, NULL);

    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_log(task, NXT_LOG_ERR, "failed to send READY message to main");

        goto fail;
    }

    return;

fail:

    exit(1);
}


#if (NXT_HAVE_POSIX_SPAWN)

/*
 * Linux glibc 2.2 posix_spawn() is implemented via fork()/execve().
 * Linux glibc 2.4 posix_spawn() without file actions and spawn
 * attributes uses vfork()/execve().
 *
 * On FreeBSD 8.0 posix_spawn() is implemented via vfork()/execve().
 *
 * Solaris 10:
 *   In the Solaris 10 OS, posix_spawn() is currently implemented using
 *   private-to-libc vfork(), execve(), and exit() functions.  They are
 *   identical to regular vfork(), execve(), and exit() in functionality,
 *   but they are not exported from libc and therefore don't cause the
 *   deadlock-in-the-dynamic-linker problem that any multithreaded code
 *   outside of libc that calls vfork() can cause.
 *
 * On MacOSX 10.5 (Leoprad) and NetBSD 6.0 posix_spawn() is implemented
 * as syscall.
 */

nxt_pid_t
nxt_process_execute(nxt_task_t *task, char *name, char **argv, char **envp)
{
    nxt_pid_t  pid;

    nxt_debug(task, "posix_spawn(\"%s\")", name);

    if (posix_spawn(&pid, name, NULL, NULL, argv, envp) != 0) {
        nxt_log(task, NXT_LOG_CRIT, "posix_spawn(\"%s\") failed %E",
                name, nxt_errno);
        return -1;
    }

    return pid;
}

#else

nxt_pid_t
nxt_process_execute(nxt_task_t *task, char *name, char **argv, char **envp)
{
    nxt_pid_t  pid;

    /*
     * vfork() is better than fork() because:
     *   it is faster several times;
     *   its execution time does not depend on private memory mapping size;
     *   it has lesser chances to fail due to the ENOMEM error.
     */

    pid = vfork();

    switch (pid) {

    case -1:
        nxt_log(task, NXT_LOG_CRIT, "vfork() failed while executing \"%s\" %E",
                name, nxt_errno);
        break;

    case 0:
        /* A child. */
        nxt_debug(task, "execve(\"%s\")", name);

        (void) execve(name, argv, envp);

        nxt_log(task, NXT_LOG_CRIT, "execve(\"%s\") failed %E",
                name, nxt_errno);

        exit(1);
        nxt_unreachable();
        break;

    default:
        /* A parent. */
        nxt_debug(task, "vfork(): %PI", pid);
        break;
    }

    return pid;
}

#endif


nxt_int_t
nxt_process_daemon(nxt_task_t *task)
{
    nxt_fd_t      fd;
    nxt_pid_t     pid;
    const char    *msg;

    /*
     * fork() followed by a parent process's exit() detaches a child process
     * from an init script or terminal shell process which has started the
     * parent process and allows the child process to run in background.
     */

    pid = fork();

    switch (pid) {

    case -1:
        msg = "fork() failed %E";
        goto fail;

    case 0:
        /* A child. */
        break;

    default:
        /* A parent. */
        nxt_debug(task, "fork(): %PI", pid);
        exit(0);
        nxt_unreachable();
    }

    nxt_pid = getpid();

    /* Clean inherited cached thread tid. */
    task->thread->tid = 0;

    nxt_debug(task, "daemon");

    /* Detach from controlling terminal. */

    if (setsid() == -1) {
        nxt_log(task, NXT_LOG_CRIT, "setsid() failed %E", nxt_errno);
        return NXT_ERROR;
    }

    /*
     * Reset file mode creation mask: any access
     * rights can be set on file creation.
     */
    umask(0);

    /* Redirect STDIN and STDOUT to the "/dev/null". */

    fd = open("/dev/null", O_RDWR);
    if (fd == -1) {
        msg = "open(\"/dev/null\") failed %E";
        goto fail;
    }

    if (dup2(fd, STDIN_FILENO) == -1) {
        msg = "dup2(\"/dev/null\", STDIN) failed %E";
        goto fail;
    }

    if (dup2(fd, STDOUT_FILENO) == -1) {
        msg = "dup2(\"/dev/null\", STDOUT) failed %E";
        goto fail;
    }

    if (fd > STDERR_FILENO) {
        nxt_fd_close(fd);
    }

    return NXT_OK;

fail:

    nxt_log(task, NXT_LOG_CRIT, msg, nxt_errno);

    return NXT_ERROR;
}


void
nxt_nanosleep(nxt_nsec_t ns)
{
    struct timespec  ts;

    ts.tv_sec = ns / 1000000000;
    ts.tv_nsec = ns % 1000000000;

    (void) nanosleep(&ts, NULL);
}


nxt_int_t
nxt_user_cred_get(nxt_task_t *task, nxt_user_cred_t *uc, const char *group)
{
    struct group   *grp;
    struct passwd  *pwd;

    nxt_errno = 0;

    pwd = getpwnam(uc->user);

    if (nxt_slow_path(pwd == NULL)) {

        if (nxt_errno == 0) {
            nxt_log(task, NXT_LOG_CRIT,
                    "getpwnam(\"%s\") failed, user \"%s\" not found",
                    uc->user, uc->user);
        } else {
            nxt_log(task, NXT_LOG_CRIT, "getpwnam(\"%s\") failed %E",
                    uc->user, nxt_errno);
        }

        return NXT_ERROR;
    }

    uc->uid = pwd->pw_uid;
    uc->base_gid = pwd->pw_gid;

    if (group != NULL && group[0] != '\0') {
        nxt_errno = 0;

        grp = getgrnam(group);

        if (nxt_slow_path(grp == NULL)) {

            if (nxt_errno == 0) {
                nxt_log(task, NXT_LOG_CRIT,
                        "getgrnam(\"%s\") failed, group \"%s\" not found",
                        group, group);
            } else {
                nxt_log(task, NXT_LOG_CRIT, "getgrnam(\"%s\") failed %E",
                        group, nxt_errno);
            }

            return NXT_ERROR;
        }

        uc->base_gid = grp->gr_gid;
    }

    if (getuid() == 0) {
        return nxt_user_groups_get(task, uc);
    }

    return NXT_OK;
}


/*
 * nxt_user_groups_get() stores an array of groups IDs which should be
 * set by the initgroups() function for a given user.  The initgroups()
 * may block a just forked worker process for some time if LDAP or NDIS+
 * is used, so nxt_user_groups_get() allows to get worker user groups in
 * main process.  In a nutshell the initgroups() calls getgrouplist()
 * followed by setgroups().  However Solaris lacks the getgrouplist().
 * Besides getgrouplist() does not allow to query the exact number of
 * groups while NGROUPS_MAX can be quite large (e.g. 65536 on Linux).
 * So nxt_user_groups_get() emulates getgrouplist(): at first the function
 * saves the super-user groups IDs, then calls initgroups() and saves the
 * specified user groups IDs, and then restores the super-user groups IDs.
 * This works at least on Linux, FreeBSD, and Solaris, but does not work
 * on MacOSX, getgroups(2):
 *
 *   To provide compatibility with applications that use getgroups() in
 *   environments where users may be in more than {NGROUPS_MAX} groups,
 *   a variant of getgroups(), obtained when compiling with either the
 *   macros _DARWIN_UNLIMITED_GETGROUPS or _DARWIN_C_SOURCE defined, can
 *   be used that is not limited to {NGROUPS_MAX} groups.  However, this
 *   variant only returns the user's default group access list and not
 *   the group list modified by a call to setgroups(2).
 *
 * For such cases initgroups() is used in worker process as fallback.
 */

static nxt_int_t
nxt_user_groups_get(nxt_task_t *task, nxt_user_cred_t *uc)
{
    int        nsaved, ngroups;
    nxt_int_t  ret;
    nxt_gid_t  *saved;

    nsaved = getgroups(0, NULL);

    if (nsaved == -1) {
        nxt_log(task, NXT_LOG_CRIT, "getgroups(0, NULL) failed %E", nxt_errno);
        return NXT_ERROR;
    }

    nxt_debug(task, "getgroups(0, NULL): %d", nsaved);

    if (nsaved > NGROUPS_MAX) {
        /* MacOSX case. */
        return NXT_OK;
    }

    saved = nxt_malloc(nsaved * sizeof(nxt_gid_t));

    if (saved == NULL) {
        return NXT_ERROR;
    }

    ret = NXT_ERROR;

    nsaved = getgroups(nsaved, saved);

    if (nsaved == -1) {
        nxt_log(task, NXT_LOG_CRIT, "getgroups(%d) failed %E",
                nsaved, nxt_errno);
        goto fail;
    }

    nxt_debug(task, "getgroups(): %d", nsaved);

    if (initgroups(uc->user, uc->base_gid) != 0) {
        nxt_log(task, NXT_LOG_CRIT, "initgroups(%s, %d) failed",
                             uc->user, uc->base_gid);
        goto restore;
    }

    ngroups = getgroups(0, NULL);

    if (ngroups == -1) {
        nxt_log(task, NXT_LOG_CRIT, "getgroups(0, NULL) failed %E", nxt_errno);
        goto restore;
    }

    nxt_debug(task, "getgroups(0, NULL): %d", ngroups);

    uc->gids = nxt_malloc(ngroups * sizeof(nxt_gid_t));

    if (uc->gids == NULL) {
        goto restore;
    }

    ngroups = getgroups(ngroups, uc->gids);

    if (ngroups == -1) {
        nxt_log(task, NXT_LOG_CRIT, "getgroups(%d) failed %E",
                ngroups, nxt_errno);
        goto restore;
    }

    uc->ngroups = ngroups;

#if (NXT_DEBUG)
    {
        u_char      *p, *end;
        nxt_uint_t  i;
        u_char      msg[NXT_MAX_ERROR_STR];

        p = msg;
        end = msg + NXT_MAX_ERROR_STR;

        for (i = 0; i < uc->ngroups; i++) {
            p = nxt_sprintf(p, end, "%uL:", (uint64_t) uc->gids[i]);
        }

        nxt_debug(task, "user \"%s\" cred: uid:%uL base gid:%uL, gids:%*s",
                  uc->user, (uint64_t) uc->uid, (uint64_t) uc->base_gid,
                  p - msg, msg);
    }
#endif

    ret = NXT_OK;

restore:

    if (setgroups(nsaved, saved) != 0) {
        nxt_log(task, NXT_LOG_CRIT, "setgroups(%d) failed %E",
                nsaved, nxt_errno);
        ret = NXT_ERROR;
    }

fail:

    nxt_free(saved);

    return ret;
}


nxt_int_t
nxt_user_cred_set(nxt_task_t *task, nxt_user_cred_t *uc)
{
    nxt_debug(task, "user cred set: \"%s\" uid:%uL base gid:%uL",
              uc->user, (uint64_t) uc->uid, uc->base_gid);

    if (setgid(uc->base_gid) != 0) {
        nxt_log(task, NXT_LOG_CRIT, "setgid(%d) failed %E",
                uc->base_gid, nxt_errno);
        return NXT_ERROR;
    }

    if (uc->gids != NULL) {
        if (setgroups(uc->ngroups, uc->gids) != 0) {
            nxt_log(task, NXT_LOG_CRIT, "setgroups(%i) failed %E",
                    uc->ngroups, nxt_errno);
            return NXT_ERROR;
        }

    } else {
        /* MacOSX fallback. */
        if (initgroups(uc->user, uc->base_gid) != 0) {
            nxt_log(task, NXT_LOG_CRIT, "initgroups(%s, %d) failed",
                    uc->user, uc->base_gid);
            return NXT_ERROR;
        }
    }

    if (setuid(uc->uid) != 0) {
        nxt_log(task, NXT_LOG_CRIT, "setuid(%d) failed %E", uc->uid, nxt_errno);
        return NXT_ERROR;
    }

    return NXT_OK;
}


static void
nxt_process_port_mp_cleanup(nxt_task_t *task, void *obj, void *data)
{
    nxt_runtime_t  *rt;
    nxt_process_t  *process;

    process = obj;
    rt = data;

    process->port_cleanups--;

    if (process->port_cleanups == 0) {
        nxt_runtime_process_remove(rt, process);
    }
}

void
nxt_process_port_add(nxt_task_t *task, nxt_process_t *process, nxt_port_t *port)
{
    port->process = process;
    nxt_queue_insert_tail(&process->ports, &port->link);

    nxt_mp_cleanup(port->mem_pool, nxt_process_port_mp_cleanup, task, process,
                   task->thread->runtime);
    process->port_cleanups++;
}


void
nxt_process_connected_port_add(nxt_process_t *process, nxt_port_t *port)
{
    nxt_thread_mutex_lock(&process->cp_mutex);

    if (process->cp_mem_pool == NULL) {
        process->cp_mem_pool = nxt_mp_create(1024, 128, 256, 32);
    }

    nxt_mp_thread_adopt(process->cp_mem_pool);

    nxt_port_hash_add(&process->connected_ports, process->cp_mem_pool, port);

    nxt_thread_mutex_unlock(&process->cp_mutex);
}

void
nxt_process_connected_port_remove(nxt_process_t *process, nxt_port_t *port)
{
    nxt_thread_mutex_lock(&process->cp_mutex);

    if (process->cp_mem_pool != NULL) {
        nxt_mp_thread_adopt(process->cp_mem_pool);

        nxt_port_hash_remove(&process->connected_ports, process->cp_mem_pool,
                             port);
    }

    nxt_thread_mutex_unlock(&process->cp_mutex);
}

nxt_port_t *
nxt_process_connected_port_find(nxt_process_t *process, nxt_pid_t pid,
    nxt_port_id_t port_id)
{
    nxt_port_t  *res;

    nxt_thread_mutex_lock(&process->cp_mutex);

    res = nxt_port_hash_find(&process->connected_ports, pid, port_id);

    nxt_thread_mutex_unlock(&process->cp_mutex);

    return res;
}
