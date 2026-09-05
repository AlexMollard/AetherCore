# Build context is THIS directory (tools/rendezvous/), not the repo root - see
# docker-compose.yml and the README's deploy steps. Two stages: the build stage
# fetches ENet and compiles main.cpp + RendezvousServerLogic.hpp; the runtime stage
# keeps nothing but the resulting binary, so a compiler and a copy of ENet's source
# never ship to a public-facing host.
#
# Builds through docker/CMakeLists.txt, a standalone CMake project - NOT the
# CMakeLists.txt beside this file, which is meant to be add_subdirectory()'d from
# the AetherCore engine root and would drag in a required Vulkan SDK this UDP-only
# server never touches (see CMake/Dependencies.cmake). See docker/CMakeLists.txt's
# own comment for why a second CMake project exists at all.
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
		cmake g++ git ca-certificates \
	&& rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY main.cpp RendezvousServerLogic.hpp ./
COPY docker/CMakeLists.txt ./docker/CMakeLists.txt
RUN cmake -S docker -B build -DCMAKE_BUILD_TYPE=Release \
	&& cmake --build build --target aether-rendezvous -j"$(nproc)"

# Runtime: the binary, nothing else. debian-slim rather than a static/scratch image
# because ENet on Linux still links glibc; this stage carries only that shared libc,
# no compiler and no source - what a public UDP endpoint needs and nothing it doesn't.
FROM debian:bookworm-slim AS runtime
RUN useradd --system --no-create-home --shell /usr/sbin/nologin rendezvous
COPY --from=build /src/build/aether-rendezvous /usr/local/bin/aether-rendezvous
USER rendezvous
# Candidate blobs only, never game traffic - see ../README.md's abuse bounds for
# exactly what this open port does and does not accept.
EXPOSE 24701/udp
ENTRYPOINT ["/usr/local/bin/aether-rendezvous"]
