WElyOS root file system
-----------------------
/bin/init      the test program (built from user/init.c)
/bin/echo      prints its arguments
/bin/cat       prints files
/bin/ls        lists a directory
/bin/mkdir     creates directories
/bin/rm        removes files and empty directories
/bin/sh        the shell
/etc/test.sh   shell script used by the self-test
/etc/motd      message shown by the file system self-test
/dev/console   the console, as a device node (created by the kernel at boot)

Everything here is packed into rootfs.tar at build time and embedded in the kernel image.
