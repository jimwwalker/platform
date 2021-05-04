/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#include <phosphor/phosphor.h>
#include <platform/getopt.h>
#include <platform/platform_thread.h>
#include <win32/getopt.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

struct thread_execute {
    cb_thread_main_func func;
    void *argument;
    // Windows doesn't support naming threads, but phosphor does
    std::string thread_name;
};

static DWORD WINAPI platform_thread_wrap(LPVOID arg)
{
    auto *ctx = reinterpret_cast<struct thread_execute*>(arg);
    assert(ctx);
    PHOSPHOR_INSTANCE.registerThread(ctx->thread_name);
    ctx->func(ctx->argument);
    PHOSPHOR_INSTANCE.deregisterThread();
    delete ctx;
    return 0;
}

int cb_create_thread(cb_thread_t *id,
                     void (*func)(void *arg),
                     void *arg,
                     int detached)
{
    // Implemented in terms of cb_create_named_thread; without a name.
    return cb_create_named_thread(id, func, arg, detached, NULL);
}

int cb_create_named_thread(cb_thread_t* id,
                                               void (*func)(void* arg),
                                               void* arg,
                                               int detached,
                                               const char* name) {
    HANDLE handle;

    struct thread_execute *ctx;
    try {
        ctx = new struct thread_execute;
    } catch (std::bad_alloc) {
        return -1;
    }

    ctx->func = func;
    ctx->argument = arg;
    ctx->thread_name = (name == nullptr) ? "" : name;

    handle = CreateThread(NULL, 0, platform_thread_wrap, ctx, 0, id);
    if (handle == NULL) {
        delete ctx;
        return -1;
    } else {
        if (detached) {
            CloseHandle(handle);
        }
    }

    return 0;
}

int cb_join_thread(cb_thread_t id)
{
    // We've seen problems where we've had global std::unique_ptr's which
    // had to run destructors which waited for threads be run on a "random"
    // thread causing a deadlock (the actual use was in memcached with the
    // parent monitor thread). The C++ runtime just picked a random thread
    // to run these destructors, and sometimes the parent monitor thread
    // would run them. We've refactored the code in memcached so that object
    // is no longer global, but to avoid facing the problem at a later time
    // we should add a guard here. (and it doesn't make any sense from
    // a logical perspective to wait for the current thread to be done ;-)
    if (cb_thread_self() == id) {
        throw std::runtime_error("cb_join_thread: can't try to join self");
    }

    HANDLE handle = OpenThread(SYNCHRONIZE, FALSE, id);
    if (handle == NULL) {
        return -1;
    }
    WaitForSingleObject(handle, INFINITE);
    CloseHandle(handle);
    return 0;
}

cb_thread_t cb_thread_self(void)
{
    return GetCurrentThreadId();
}

int cb_set_thread_name(const char*)
{
    // Not implemented on WIN32
    return -1;
}

int cb_get_thread_name(char*, size_t)
{
    return -1;
}

bool is_thread_name_supported(void)
{
    return false;
}

void cb_rw_lock_initialize(cb_rwlock_t *rw)
{
    InitializeSRWLock(rw);
}

void cb_rw_lock_destroy(cb_rwlock_t *rw)
{
    (void)rw;
    // Nothing todo on windows
}

int cb_rw_reader_enter(cb_rwlock_t *rw)
{
    AcquireSRWLockShared(rw);
    return 0;
}

int cb_rw_reader_exit(cb_rwlock_t *rw)
{
    ReleaseSRWLockShared(rw);
    return 0;
}

int cb_rw_writer_enter(cb_rwlock_t *rw)
{
    AcquireSRWLockExclusive(rw);
    return 0;
}

int cb_rw_writer_exit(cb_rwlock_t *rw)
{
    ReleaseSRWLockExclusive(rw);
    return 0;
}

// Wrapper into cb::getopt (which we now unit tests on all platforms)
char* optarg;
int opterr;
int optind = 1;
int optopt;

int getopt_long(int argc,
                char** argv,
                const char* optstring,
                const struct option* longopts,
                int* longindex) {
    cb::getopt::optind = optind;
    cb::getopt::opterr = opterr;
    cb::getopt::optopt = optopt;
    auto ret = cb::getopt::getopt_long(
            argc,
            argv,
            optstring,
            reinterpret_cast<const cb::getopt::option*>(longopts),
            longindex);
    optarg = cb::getopt::optarg;
    opterr = cb::getopt::opterr;
    optind = cb::getopt::optind;
    optopt = cb::getopt::optopt;
    return ret;
}

int getopt(int argc, char** argv, const char* optstring) {
    cb::getopt::optind = optind;
    cb::getopt::opterr = opterr;
    cb::getopt::optopt = optopt;
    auto ret = cb::getopt::getopt(argc, argv, optstring);
    optarg = cb::getopt::optarg;
    opterr = cb::getopt::opterr;
    optind = cb::getopt::optind;
    optopt = cb::getopt::optopt;
    return ret;
}
