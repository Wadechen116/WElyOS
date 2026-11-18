WElyOS root file system
-----------------------
/bin/init      the test program (built from user/init.c)
/bin/echo      prints its arguments
/bin/cat       prints files
/bin/ls        lists a directory (ls -l shows type, size and modification time)
/bin/date      shows the date and time (UTC; "date +8" adds a time-zone offset)
/bin/mkdir     creates directories
/bin/rm        removes files and empty directories
/bin/mv        renames or moves a file or directory (same file system only)
/bin/wc        counts lines, words and bytes
/bin/sort      sorts lines (sort FILE, or at the end of a pipe; -r reverses)
/bin/sleep     waits (sleep 3, sleep 200ms)
/bin/kill      sends a signal (kill PID, kill -9 PID, kill -STOP PID, kill -CONT PID)
/bin/ps        lists the processes
/bin/top       live view of CPUs, memory and processes (q quits; top -b prints once)
/bin/cp        copies a file (cp /bin/echo /disk/echo)
/bin/df        shows how full the file systems are
/bin/sync      writes pending changes to the disk now
/bin/id        shows who you are (uid, gid) and which privileges you hold
/bin/su        starts a shell as another user (su alice); exit returns
/bin/chmod     changes permissions (chmod 755 FILE); chown changes the owner (root only)
/bin/ping      checks that another machine answers (ping 10.0.2.2); net shows the network state
/bin/tftp      fetches a file from the host (tftp etc/motd /disk/motd)
/bin/host      looks up the address of a name (host example.com)
/bin/faultlog  shows the black box: events kept on disk across reboots (panics, faults, restarts)
/bin/scrub     reads every disk block and verifies its checksum
/bin/supervise restarts a command when it fails (supervise -n 3 COMMAND)
/bin/sh        the shell (pipes, redirection, background jobs with &, Ctrl-Z, fg, bg)
/etc/test.sh   shell script used by the self-test
/etc/motd      message shown by the file system self-test
/etc/passwd    user names and numbers (name:uid:gid); there are no passwords
/dev/console   the console, as a device node (created by the kernel at boot)
/dev/null      reads as empty, discards whatever is written to it
/disk          the disk, if one is attached: files here are kept across reboots

Everything here is packed into rootfs.tar at build time and embedded in the kernel image.
