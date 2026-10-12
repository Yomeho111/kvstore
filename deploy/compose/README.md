# Docker Compose Deployment

Run the three services without Kubernetes or a remote image registry. This
stack reuses the Dockerfiles in `deploy/docker/`; no business-code changes are
required. Files have only been statically checked: images were not built and
containers were not started or deployment-tested.

## Files

```text
deploy/compose/
  compose.yaml
  .env.example
  README.md
  config/
    kvstore.ini
    vectordb.ini
    server_config.json
```

Use Docker with BuildKit and Docker Compose v2.20 or newer. The target platform
is Linux amd64. Source submodules must be populated, and the host or Docker VM
must support io_uring. First-time builds/downloads need internet access and
enough disk space; CPU model inference needs sufficient RAM. The memory limits
are starting examples, not workload sizing guarantees.

## Start

Run these commands from the repository root:

```bash
docker compose -f deploy/compose/compose.yaml config --quiet
docker compose -f deploy/compose/compose.yaml build
docker compose -f deploy/compose/compose.yaml up -d
docker compose -f deploy/compose/compose.yaml ps
docker compose -f deploy/compose/compose.yaml logs -f vectordb-service
```

The build creates local `kvstore:dev`, `vectordb:dev` and
`vectordb-service:dev` images. The databases use different `ENABLE_RAG` values
(`OFF` for ordinary KV, `ON` for vectors). `pull_policy: never` prevents pulling
these application tags from a registry, but uncached base images and packages
still need downloads during the build. `up -d --build` can combine build/start
when rebuilding. Build paths resolve relative to this Compose file.

The service waits for both database TCP health checks. Model initialization
can then download weights, tokenizer data and custom code for
`Alibaba-NLP/gte-multilingual-base`; it has a 30-minute initial health grace
period. Health checks only inspect listening ports, not application semantics.
Docker does not automatically restart a process solely because it becomes
unhealthy. An exited process is restarted by `unless-stopped`.

## Connect And Configure

Connect a gRPC client to `127.0.0.1:50051`; no `kubectl port-forward` is needed.
Search requests should supply both `topk` and a distance `threshold`.
The two database ports are not published to the host. Inside the bridge network,
the service connects to `kvstore:8050` and `vectordb:8060` through Docker DNS.
The network permits outbound access for model downloads.

The JSON supplies connection-pool settings; each INI enables AOF and standalone
operation. The Compose configs are mounted read-only. Recreate affected containers
after configuration changes so processes load the new settings.

For overrides:

```bash
cp deploy/compose/.env.example deploy/compose/.env
docker compose --env-file deploy/compose/.env \
  -f deploy/compose/compose.yaml up -d
```

Edit the copied file for `GRPC_PORT`, `GRPC_BIND_ADDRESS`, `BUILD_JOBS`,
`MODEL_NAME` or `HF_HUB_OFFLINE`. The default loopback bind avoids public
exposure. Setting the bind address to `0.0.0.0` exposes an unauthenticated,
unencrypted gRPC endpoint; add transport/authentication controls first.
The environment file is optional; built-in defaults work without it.

## Persistence And Shutdown

The stack uses independent named volumes for ordinary KV data, vector data,
and the full Hugging Face cache. The databases work in `/data` and write AOF
under `/data/data/`; the cache lives at `/cache/huggingface`. Named volumes reuse
the image's directory contents/ownership when created. Existing volumes or
alternate host mounts may need permissions compatible with UID/GID 10001.

To stop and remove containers/network while retaining all volumes:

```bash
docker compose -f deploy/compose/compose.yaml down
```

`down -v` also deletes the database and model-cache volumes: use it only when
intentionally discarding all persistent data. Back up data before destructive
changes. Volumes do not add transactional consistency or fsync guarantees to
the application. Increasing replicas does not replicate either database.

The service and database processes receive SIGTERM and have 60 seconds to stop.
Dependency ordering is a startup aid, not a consistency mechanism: a backend
failure after startup still produces RPC storage errors.

## Offline Models And io_uring

For offline use, prepare a complete model/custom-code cache online and seed the
model-cache volume, then set `HF_HUB_OFFLINE=1`. Do not enable offline mode on an
empty cache. A custom local `MODEL_NAME` path requires an additional read-only
bind mount for that directory plus any needed custom-code cache. Changing models
requires re-embedding stored text; dimension equality is not sufficient.

Only the database containers use `seccomp=unconfined`, matching the existing
Kubernetes development configuration so common profiles do not deny io_uring
calls. All services run as UID 10001, drop capabilities, disable privilege
escalation and use read-only root filesystems plus writable `/tmp` mounts.
They are not privileged. Unconfined weakens syscall isolation: production should
use a reviewed seccomp profile permitting the necessary io_uring calls. A
disabled kernel feature or outer runtime policy can still block io_uring.