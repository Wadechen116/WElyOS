WElyOS root file system
-----------------------
/bin/init      the test program (built from user/init.c)
/bin/echo      prints its arguments
/bin/cat       prints files
/bin/ls        lists a directory
/bin/mkdir     creates directories
/bin/rm        removes files and empty directories
/bin/wc        counts lines, words and bytes
/bin/sleep     waits (sleep 3, sleep 200ms)
/bin/kill      sends a signal (kill PID, kill -9 PID, kill -STOP PID, kill -CONT PID)
/bin/ps        lists the processes
/bin/top       live view of CPUs, memory and processes (q quits; top -b prints once)
/bin/cp        copies a file (cp /bin/echo /disk/echo)
/bin/df        shows how full the file systems are
/bin/sync      writes pending changes to the disk now
/bin/sh        the shell (pipes, redirection, background jobs with &, Ctrl-Z, fg, bg)
/etc/test.sh   shell script used by the self-test
/etc/motd      message shown by the file system self-test
/dev/console   the console, as a device node (created by the kernel at boot)
/dev/null      reads as empty, discards whatever is written to it
/disk          the disk, if one is attached: files here are kept across reboots

Everything here is packed into rootfs.tar at build time and embedded in the kernel image.
