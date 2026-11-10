# The delegate worker for this repo: the generic worker (local-delegate's delegate-worker:local)
# plus what gorb-os's own gates need, matching CI (.github/workflows/ci.yml):
#   - clang 21 and lld 21 from apt.llvm.org: the apps build to wasm with them, and the shelf
#     check is byte-for-byte, so a different clang would fail it (BUILDING.md);
#   - zlib, Pillow, NumPy and tzdata for the host suites.
# Build:  docker build -t delegate-worker-gorb:local -f tools/delegate/worker.Dockerfile tools/delegate
# Use:    DELEGATE_IMAGE=delegate-worker-gorb:local delegate run|chain ...
FROM delegate-worker:local

RUN apt-get update && apt-get install -y --no-install-recommends gnupg zlib1g-dev python3-pil python3-numpy tzdata \
 && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
 && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-21 main" \
      > /etc/apt/sources.list.d/llvm-21.list \
 && apt-get update && apt-get install -y --no-install-recommends clang-21 lld-21 \
 && rm -rf /var/lib/apt/lists/* \
 && /usr/lib/llvm-21/bin/clang --version | head -1

ENV ORB_CLANG=/usr/lib/llvm-21/bin/clang \
    PATH=/usr/lib/llvm-21/bin:$PATH
