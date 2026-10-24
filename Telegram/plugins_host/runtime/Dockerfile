FROM rockylinux:8

RUN dnf -q -y install dnf-plugins-core \
 && dnf config-manager --set-enabled powertools \
 && dnf -q -y install gcc-toolset-13-gcc-c++ gcc-toolset-13-binutils cmake make \
    openssl-devel zlib-devel llvm-toolset lld cpio \
 && dnf clean all

RUN mkdir -p /sysroot/aarch64/usr/lib64 /sysroot/aarch64/usr/lib /tmp/rpms \
 && ln -s usr/lib64 /sysroot/aarch64/lib64 \
 && ln -s usr/lib /sysroot/aarch64/lib \
 && cd /tmp/rpms \
 && dnf -q download --forcearch=aarch64 glibc glibc-devel glibc-headers kernel-headers \
    libgcc libstdc++ gcc-toolset-13-gcc gcc-toolset-13-libstdc++-devel \
    openssl-libs openssl-devel zlib zlib-devel \
 && for rpm in *.rpm; do rpm2cpio "$rpm" | (cd /sysroot/aarch64 && cpio -idm --quiet); done \
 && rm -rf /tmp/rpms \
 && dnf clean all
