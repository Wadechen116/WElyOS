# Shell script run by the kernel self-test (sh /etc/test.sh).
# The kernel checks the exit code and the file this script leaves behind.
echo "  [sh] script started"
ls /bin
mkdir /work
echo first line > /work/result
echo "second line, with  two  spaces" >> /work/result
cat /work/result
# pipes and input redirection
ls /bin | wc
cat /etc/motd | cat | wc >> /work/result
wc < /etc/motd
echo too | many | | pipes
# background jobs: the shell does not wait; "wait" collects them
sleep 30ms &
sleep 20ms | cat &
jobs
ps
top -b
wait
jobs
nosuchcommand &
fg 9
bg
nosuchcommand
status
cat /does/not/exist
status
rm /work
exit 7
