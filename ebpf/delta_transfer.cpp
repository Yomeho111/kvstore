#include "delta.h"

#include <sys/resource.h>
#include <bpf/libbpf.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>

#include "transfer_worker.h"

#include "delta_transfer.skel.h"

static volatile sig_atomic_t exiting = 0;

static void sig_int(int signo)
{
    (void)signo;
    exiting = 1;
}

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
    if (level == LIBBPF_DEBUG)
        return 0;

    return vfprintf(stderr, format, args);
}

static int trans_sample_event(void *ctx, void *data, size_t data_size)
{
    (void)ctx;

    if (data_size < sizeof(struct SliceInfo))
        return 0;

    struct SliceInfo *info = (struct SliceInfo *)data;
    size_t size = info->size;

    if (size == 0 || size > INFO_BUFFER_SIZE)
        return 0;

    auto slice = std::make_unique_for_overwrite<SliceInfo>();

    slice->addr_sport = info->addr_sport;
    slice->size = size;
    memcpy(slice->buf, info->buf, size);

    delta::TransferWorker::instance().submit_slice(std::move(slice));

    return 0;
}

// Either a numeric pid or the comm of the running server, e.g. `delta_transfer kvstore`.
static pid_t resolve_target_pid(const char *arg)
{
    char *end = nullptr;
    long val = strtol(arg, &end, 10);

    if (end != arg && *end == '\0')
        return val > 0 ? static_cast<pid_t>(val) : -1;

    DIR *proc = opendir("/proc");
    if (!proc)
        return -1;

    pid_t found = -1;
    struct dirent *ent = nullptr;

    while ((ent = readdir(proc)) != nullptr)
    {
        if (!isdigit(static_cast<unsigned char>(ent->d_name[0])))
            continue;

        char path[288];
        snprintf(path, sizeof(path), "/proc/%s/comm", ent->d_name);

        FILE *fp = fopen(path, "r");
        if (!fp)
            continue;

        char comm[256]{0};
        if (fgets(comm, sizeof(comm), fp))
        {
            comm[strcspn(comm, "\n")] = '\0';
            if (strcmp(comm, arg) == 0)
                found = static_cast<pid_t>(atoi(ent->d_name));
        }
        fclose(fp);

        if (found > 0)
            break;
    }

    closedir(proc);
    return found;
}

int main(int argc, char **argv)
{
    struct delta_transfer_bpf *skel = nullptr;
    struct ring_buffer *rb = nullptr;
    pid_t target_pid = -1;
    int err = 0;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <target-pid|target-comm>\n", argv[0]);
        return 1;
    }

    target_pid = resolve_target_pid(argv[1]);
    if (target_pid <= 0)
    {
        fprintf(stderr, "Cannot resolve target process '%s'\n", argv[1]);
        return 1;
    }

    libbpf_set_print(libbpf_print_fn);

    skel = delta_transfer_bpf__open();
    if (!skel)
    {
        fprintf(stderr, "Failed to open BPF skeleton\n");
        return 1;
    }

    // read-only data is frozen by the loader, so it has to be set before load
    skel->rodata->target_pid = target_pid;

    err = delta_transfer_bpf__load(skel);
    if (err)
    {
        fprintf(stderr, "Failed to load BPF skeleton: %d\n", err);
        goto cleanup;
    }

    err = delta_transfer_bpf__attach(skel);
    if (err)
    {
        fprintf(stderr, "Failed to attach BPF skeleton: %d\n", err);
        goto cleanup;
    }

    rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), trans_sample_event, nullptr, nullptr);
    if (!rb)
    {
        err = -1;
        fprintf(stderr, "Failed to create ring buffer\n");
        goto cleanup;
    }

    if (signal(SIGINT, sig_int) == SIG_ERR || signal(SIGTERM, sig_int) == SIG_ERR)
    {
        err = -errno;
        fprintf(stderr, "Can't set signal handler: %s\n", strerror(errno));
        goto cleanup;
    }

    // printf("delta transfer attached to pid %d\n", target_pid);

    while (!exiting)
    {
        err = ring_buffer__poll(rb, 100);
        if (err == -EINTR)
        {
            err = 0;
            break;
        }

        if (err < 0)
        {
            fprintf(stderr, "Error polling ring buffer: %d\n", err);
            break;
        }

        err = 0;
    }

cleanup:
    ring_buffer__free(rb);
    delta_transfer_bpf__destroy(skel);
    return err < 0 ? -err : 0;
}
