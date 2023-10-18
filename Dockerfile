FROM ubuntu:22.04

ARG ZEPHYR_VERSION=0.16.3

ARG TARGETPLATFORM

ENV PATH="${PATH}:/root/.local/bin"

RUN apt update && apt install -y \
    git \
    pkg-config \
    unzip \
    build-essential \
    python3-pip \
    libsdl2-dev \
    libcodec2-dev \
    libreadline-dev \
    wget \
    libusb-1.0-0 \
    libusb-1.0-0-dev \
    dfu-util \
    gperf \
    ccache \
    device-tree-compiler \
    xz-utils

RUN pip3 install --user meson ninja cmake \
                        west cbor2 pyelftools \
                        intelhex requests pyserial

RUN if [ "$TARGETPLATFORM" = "linux/amd64" ]  ;\
        then ARCHITECTURE=x86_64              ;\
    elif [ "$TARGETPLATFORM" = "linux/arm64" ];\
        then ARCHITECTURE=aarch64             ;\
    else ARCHITECTURE=x86_64                  ;\
    fi && \
    wget https://files.openrtx.org/toolchains/arm-miosix-eabi-${ARCHITECTURE}.zip \
            -O /tmp/miosix.zip

RUN unzip /tmp/miosix.zip -d /opt/ && \
    ln -s /opt/arm-miosix-eabi/bin/* /usr/bin && \
    rm /tmp/miosix.zip

RUN if [ "$TARGETPLATFORM" = "linux/amd64" ]  ;\
        then ARCHITECTURE=x86_64              ;\
    elif [ "$TARGETPLATFORM" = "linux/arm64" ];\
        then ARCHITECTURE=aarch64             ;\
    else ARCHITECTURE=x86_64                  ;\
    fi && \
    wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_VERSION}/zephyr-sdk-${ZEPHYR_VERSION}_linux-${ARCHITECTURE}_minimal.tar.xz \
            -O /tmp/zephyr.tar.xz 

RUN tar xf /tmp/zephyr.tar.xz -C /opt/

RUN cd /opt/zephyr* && \
    ./setup.sh -t xtensa-espressif_esp32s3_zephyr-elf -c
