# Match the x86-64 Arch Linux development environment.
FROM archlinux:base

ARG ARCH_SNAPSHOT=2026/11/20
# Keep downloads in the builder cache across retries and rebuilds.
RUN --mount=type=cache,id=boaros-arch-packages,target=/var/cache/pacman/pkg,sharing=locked \
    printf 'Server = https://archive.archlinux.org/repos/%s/$repo/os/$arch\n' "$ARCH_SNAPSHOT" \
        > /etc/pacman.d/mirrorlist \
    && pacman -Syyuu --noconfirm --needed --disable-download-timeout \
        gcc make git python curl e2fsprogs pahole \
        riscv64-elf-gcc riscv64-elf-binutils riscv64-linux-gnu-binutils \
        qemu-system-riscv qemu-system-riscv-firmware qemu-system-loongarch64

# The LoongArch cross compiler is the same upstream release used by the host.
COPY references/sources.tsv /tmp/boaros-sources.tsv
RUN set -eu; \
    url="$(awk -F '\t' '$2 == "toolchains/la-gcc15-glibc242.tar.xz" { print $3 }' /tmp/boaros-sources.tsv)"; \
    digest="$(awk -F '\t' '$2 == "toolchains/la-gcc15-glibc242.tar.xz" { print $5 }' /tmp/boaros-sources.tsv)"; \
    curl -fsSL --retry 3 "$url" -o /tmp/loongarch.tar.xz; \
    printf '%s  /tmp/loongarch.tar.xz\n' "$digest" | sha256sum -c -; \
    mkdir -p /opt/loongarch64-tools; \
    tar -xJf /tmp/loongarch.tar.xz --strip-components=1 -C /opt/loongarch64-tools; \
    rm /tmp/loongarch.tar.xz /tmp/boaros-sources.tsv

ENV PATH="/opt/loongarch64-tools/bin:${PATH}" \
    PAHOLE=/usr/bin/pahole \
    QEMU_LOONGARCH64=/usr/bin/qemu-system-loongarch64
WORKDIR /workspace
