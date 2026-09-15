/* Included after the production watchdog section by watchdog_bench.py. */
static double bench_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

#ifdef WATCHDOG_WAKE_TESTS
static void clear_flags(void) { g_interrupted = 0; g_tool_timed_out = 0; }
static int inspect_timeout(void) {
    watchdog_info_t info;
    assert(watchdog_active_snapshot(&info, 1) == 1);
    return info.timed_out;
}
static void wait_soft_timeout(void) {
    double end = bench_ms() + 1600;
    while (!inspect_timeout() && bench_ms() < end) usleep(1000);
    assert(inspect_timeout());
}
static void *supervise(void *unused) {
    (void)unused;
    while (!__atomic_load_n(&s_test_supervisor_done, __ATOMIC_ACQUIRE)) {
        watchdog_info_t infos[128];
        watchdog_active_snapshot(infos, 128);
        watchdog_renew_by_name("race", 1);
        usleep(50);
    }
    return NULL;
}
static void *race_worker(void *unused) {
    (void)unused;
    for (int i = 0; i < 300; i++) {
        tool_watchdog_t wd;
        watchdog_start(&wd, pthread_self(), "race", 30);
        if (i % 3 == 0) usleep(50);
        watchdog_stop(&wd);
        assert(wd.cancelled && !wd.timed_out);
        assert(watchdog_renew(&wd, 1) == 0);
    }
    return NULL;
}
static void regression(void) {
    tool_watchdog_t wd;
    watchdog_info_t info;
    clear_flags();
    for (int i = 0; i < 1000; i++) {
        watchdog_start(&wd, pthread_self(), "immediate", 30);
        watchdog_stop(&wd);
        assert(wd.cancelled && !wd.timed_out);
    }
    assert(watchdog_active_snapshot(&info, 1) == 0);
    watchdog_start(&wd, pthread_self(), "sleeping", 30);
    usleep(10000);
    double start = bench_ms();
    watchdog_stop(&wd);
    double stop_ms = bench_ms() - start;
    assert(stop_ms < 200); /* The old implementation blocks almost 500 ms. */

    pthread_t supervisor, workers[4];
    pthread_create(&supervisor, NULL, supervise, NULL);
    for (int i = 0; i < 4; i++) pthread_create(&workers[i], NULL, race_worker, NULL);
    for (int i = 0; i < 4; i++) pthread_join(workers[i], NULL);
    __atomic_store_n(&s_test_supervisor_done, 1, __ATOMIC_RELEASE);
    pthread_join(supervisor, NULL);
    assert(watchdog_active_snapshot(&info, 1) == 0);

    clear_flags();
    watchdog_start(&wd, pthread_self(), "early_renew", 1);
    assert(watchdog_renew(&wd, 1));
    usleep(1100000);
    assert(!inspect_timeout() && !g_interrupted);
    watchdog_stop(&wd);

    clear_flags();
    watchdog_start(&wd, pthread_self(), "soft_renew", 1);
    wait_soft_timeout();
    assert(g_tool_timed_out && !g_interrupted);
    assert(watchdog_renew_by_name("soft_renew", 1) == 1);
    assert(!inspect_timeout() && !g_tool_timed_out);
    assert(watchdog_active_snapshot(&info, 1) == 1 && info.renew_count == 1);
    watchdog_stop(&wd);

    clear_flags();
    watchdog_start(&wd, pthread_self(), "capped", 1);
    pthread_mutex_lock(&s_wd_registry_lock);
    wd.max_lifetime_s = 2;
    pthread_mutex_unlock(&s_wd_registry_lock);
    assert(watchdog_renew(&wd, 20));
    pthread_mutex_lock(&s_wd_registry_lock);
    assert(wd.deadline == wd.started_at + 2);
    wd.started_at -= 100; /* Exercise an already exhausted absolute cap. */
    pthread_mutex_unlock(&s_wd_registry_lock);
    assert(!watchdog_renew(&wd, 1));
    assert(!watchdog_renew(&wd, 0));
    watchdog_stop(&wd);

    const char *names[] = {"ordinary", "swarm", "swarm_collect", "map_reduce", "provider_fabric"};
    for (int i = 0; i < 5; i++) {
        clear_flags();
        watchdog_start(&wd, pthread_self(), names[i], 1);
        /* Exercise real condition deadlines on a shortened test window,
           preserving the production clock and wait implementation. */
        pthread_mutex_lock(&s_wd_registry_lock);
        wd.deadline = watchdog_now() + .015;
        wd.grace_end = wd.deadline + .025;
        pthread_cond_signal(&wd.wake);
        pthread_mutex_unlock(&s_wd_registry_lock);
        usleep(80000);
        assert(watchdog_renew(&wd, 1) == 0); /* Grace exhausted, even before stop. */
        watchdog_stop(&wd);
        assert(wd.timed_out && g_tool_timed_out);
        assert(g_interrupted == (i == 0));
        assert(watchdog_renew(&wd, 1) == 0);
    }

    clear_flags();
    s_test_cond_failure = 1;
    watchdog_start(&wd, pthread_self(), "cond_failure", 30);
    watchdog_stop(&wd);
    assert(wd.cancelled && wd.timed_out && !wd.thread_started);
    assert(watchdog_active_snapshot(&info, 1) == 0);
    s_test_cond_failure = 0;
    s_test_thread_failure = 1;
    watchdog_start(&wd, pthread_self(), "thread_failure", 30);
    watchdog_stop(&wd);
    assert(wd.cancelled && wd.timed_out && !wd.thread_started);
    assert(watchdog_active_snapshot(&info, 1) == 0);
    s_test_thread_failure = 0;
    printf("{\"regressions_passed\":true,\"sleeping_stop_ms\":%.6f,\"immediate_stops\":1000,\"concurrent_start_stops\":1200}\n", stop_ms);
}
#endif

int main(int argc, char **argv) {
#ifdef WATCHDOG_WAKE_TESTS
    if (argc == 2 && strcmp(argv[1], "--test") == 0) { regression(); return 0; }
#endif
    assert(argc == 2);
    int fd = open(argv[1], O_RDONLY);
    assert(fd >= 0);
    char data[768];
    tool_watchdog_t wd;
    double start = bench_ms();
    watchdog_start(&wd, pthread_self(), "read_workload", 30);
    /* A brief real filesystem-read workload gives the watcher an opportunity
       to enter its wait, as it does during tools lasting at least a millisecond. */
    double read_start = bench_ms();
    int reads = 0;
    do {
        assert(pread(fd, data, sizeof(data), 0) == sizeof(data));
        assert(data[0] == 'W' && data[767] == 'W');
        reads++;
    } while (bench_ms() - read_start < 1.0);
    double read_done = bench_ms();
    watchdog_stop(&wd);
    double end = bench_ms();
    close(fd);
    assert(!wd.timed_out);
    printf("{\"total_ms\":%.6f,\"stop_ms\":%.6f,\"work_ms\":%.6f,\"reads\":%d}\n", end-start, end-read_done, read_done-read_start, reads);
    return 0;
}
