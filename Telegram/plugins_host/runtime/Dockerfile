FROM rockylinux:8

RUN dnf -q -y install dnf-plugins-core \
 && dnf config-manager --set-enabled powertools \
 && dnf -q -y install gcc-toolset-13-gcc-c++ gcc-toolset-13-binutils cmake make \
    openssl-devel zlib-devel \
 && dnf clean all
