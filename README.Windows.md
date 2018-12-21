SPICE server Windows support
============================

SPICE server was ported from Unix/Linux to Windows.

Most features are present, with some exceptions:

 - Unix sockets;
 - signal handling;
 - CLOEXEC flag (completely different handling on Windows);
 - IPTOS_LOWDELAY flag (Linux specific);
 - TCP_CORK (Linux/*BSD specific).

Some features could be ported but currently are not:

 - statistics exported through mapped files. Disabled by default and mainly
   used for development;
 - filtering while recording (SPICE_WORKER_RECORD_FILTER environment).
   Recording is used for debugging or development work;
 - TCP_KEEPIDLE setting. Default is used.

To compile and use with Qemu you can follow the guide at
https://wiki.qemu.org/Hosts/W32, just pass `--enable-spice` during
configuration.

To test using a Linux cross compiler you can install Wine and run test
with

```
make LOG_COMPILE=wine check
```
