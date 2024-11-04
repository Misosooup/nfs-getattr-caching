# NFS Getattr Cache

## Description

This is a simple kernel module that implements a cache for NFS getattr calls.

## Prerequisites

* Linux kernel headers (linux-headers-$(uname -r))
* Linux kernel development tools (kernel-devel-$(uname -r))
* GCC (gcc)
* Make (make)

## Configuration

The module is configured to cache the attributes of the following paths:

```c
static const char *cached_paths[] = {
```

You can modify the `cached_paths` array to add or remove paths from the cache.

The `CACHE_TIMEOUT_MS` constant defines the cache timeout (TTL) in milliseconds.

The `CLEANUP_INTERVAL_MS` constant defines the cleanup interval in milliseconds.

The `MAX_PATH_LEN` constant defines the maximum path length.

## Build

```bash
make
```

## Install

```bash
sudo insmod nfsgetattrcache.ko
```

## Uninstall

```bash
sudo rmmod nfsgetattrcache
```
