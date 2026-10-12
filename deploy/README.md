# Local Container Deployment

These files were generated and statically reviewed only. No image build,
container startup, Kubernetes apply, or deployment test was performed.

## Docker Compose Alternative

For a single-machine deployment without Kubernetes or a registry, use
[compose/compose.yaml](compose/compose.yaml) and the instructions in
[compose/README.md](compose/README.md). It builds the same three images locally,
mounts standalone configs, and persists database data and model cache in named
volumes. From the repository root:

```bash
docker compose -f deploy/compose/compose.yaml build
docker compose -f deploy/compose/compose.yaml up -d
```

The following sections describe the existing Kubernetes option.

## Images And Workloads

| Component | Runtime image | Workload | Port |
| --- | --- | --- | --- |
| kvstore | Minimal Ubuntu 24.04 with liburing | One-replica StatefulSet | 8050 |
| vectordb | Minimal Ubuntu 24.04 with liburing | One-replica StatefulSet | 8060 |
| vectordb-service | Python 3.12 Debian slim | One-replica Deployment | 50051 |

The two database images use the same multi-stage Dockerfile with different
`ENABLE_RAG` values. Both database stages use Ubuntu 24.04, keeping the compiler
and runtime on the same glibc environment. The builder includes CMake, Ninja,
GCC, NASM and development libraries; the runtime retains only runtime packages,
the executable and ISA-L shared libraries. The official `ubuntu:24.04` image is
already a minimal container root filesystem; there is no official
`ubuntu:24.04-slim` tag. Runtime packages are installed without recommendations,
apt indexes are removed, and compiler tools are not copied. The custom memory
pool remains enabled.

The service has separate uv, compiler and runtime stages. Only its installed
virtual environment is copied to the runtime. Official CPU PyTorch wheels need
glibc, so the service retains Debian slim. This avoids compiler tools
and CUDA packages in the runtime, but PyTorch itself is still substantial.
The model is not embedded in the image; its cache is persisted on a separate PVC.

## Prerequisites

- Docker with BuildKit, kind or another local Kubernetes cluster, and kubectl.
- Linux amd64 nodes; the manifests explicitly select that architecture.
- A default StorageClass with dynamic provisioning and fsGroup-compatible volumes.
- Disk for two 10 GiB database claims and one 5 GiB model-cache claim.
- Sufficient node memory for the model, database data and the configured requests.
- A populated `third_party/` tree, including the ISA-L submodule.
- Network access to base images, Ubuntu/Debian packages, Python indexes and Hugging Face.

## Build Locally

Run from the repository root. Building is an explicit operator action:

```bash
docker build --platform linux/amd64 -f deploy/docker/Dockerfile.kvstore \
  --build-arg ENABLE_RAG=OFF -t kvstore:dev .
docker build --platform linux/amd64 -f deploy/docker/Dockerfile.kvstore \
  --build-arg ENABLE_RAG=ON -t vectordb:dev .
docker build --platform linux/amd64 -f deploy/docker/Dockerfile.service \
  -t vectordb-service:dev .
```

Database builds default to two compiler jobs; override `BUILD_JOBS` if needed.
The service honors the existing uv lock and CPU PyTorch index. Pin base images
and the uv tool image to approved digests for production builds. Neither builder
installs tools into its runtime stage. The container builds have not been verified.

## Load And Apply

No remote registry is required. For kind:

```bash
kind create cluster --name vector-stack
kind load docker-image kvstore:dev vectordb:dev vectordb-service:dev \
  --name vector-stack
kubectl apply -f deploy/k8s/namespace.yaml
kubectl apply -f deploy/k8s/
kubectl get pods,svc,pvc -n vector-stack
kubectl rollout status statefulset/kvstore -n vector-stack
kubectl rollout status statefulset/vectordb -n vector-stack
kubectl rollout status deployment/vectordb-service -n vector-stack --timeout=30m
kubectl port-forward -n vector-stack svc/vectordb-service 50051:50051
```

For minikube replace `kind load docker-image` with `minikube image load`.
All manifests use `imagePullPolicy: Never`. Every node eligible to run a Pod must
have its image loaded. For a remote registry replace the image names and policy.
After rebuilding a development tag, reload images and restart the workloads.
Namespace creation must precede the remaining manifests.

## Configuration And Persistence

`configmaps.yaml` contains both database INI files and the service JSON. Backend
addresses use the access Services `kvstore` and `vectordb`; the additional
headless Services provide the StatefulSet identities. Replication is disabled,
and both databases use AOF with one listening port. Each database works in
`/data`, with AOF records under `/data/data/` on its own PVC.

The default model is `Alibaba-NLP/gte-multilingual-base`. Its first load downloads
weights, tokenizer data and code from `Alibaba-NLP/new-impl`. The full Hugging
Face cache is stored under `/cache/huggingface` on `vectordb-model-cache`.
The startup probe permits up to 30 minutes before treating this load as failed.
For offline deployment seed the PVC with an online-prepared complete cache before
startup and add `HF_HUB_OFFLINE=1` to the service container. A standalone local
model directory also needs its custom code or corresponding cached code.
Audit remote code and pin model/code revisions before production deployment.
Changing the embedding model requires re-embedding all stored texts.

ConfigMap changes require restarting the affected workloads. Resource limits and
PVC sizes are examples; choose them based on text volume and vector count. Do not
increase database replicas without implementing replication. The service also
starts with one replica and `Recreate` because its model cache uses ReadWriteOnce;
horizontal scaling needs an appropriate cache strategy or prepackaged model.
Deleting a StatefulSet normally retains its data PVC; deleting claims discards
the persisted data. A PVC is not a substitute for fsync guarantees, backups or
transactional writes across the two databases.

## io_uring And Security

`liburing` is a userspace library; io_uring is provided by the node kernel.
The database Pods use `seccompProfile: Unconfined` to avoid common runtime
profiles blocking `io_uring_setup`, `io_uring_enter` and `io_uring_register`.
They are not privileged, run as UID 10001 and drop all Linux capabilities.
Unconfined still reduces isolation: production should use a reviewed `Localhost`
profile that allows the necessary calls instead. Cluster admission policies may
reject Unconfined and require an administrator-approved exception or profile.
An outer container restriction or disabled kernel io_uring can still block calls.
Ubuntu, liburing, or extra capabilities alone do not resolve these restrictions.

The Python service uses `RuntimeDefault` seccomp. Writable data/cache and `/tmp`
mounts allow all containers to use read-only root filesystems. Readiness and
liveness use TCP because the service does not implement standard gRPC health;
these probes do not prove database operation correctness. Expose the gRPC
listener only on trusted networks or add TLS/authentication before public use.