# Shell script run by the kernel self-test (sh /etc/test.sh).
# The kernel checks the exit code and the file this script leaves behind.
echo "  [sh] script started"
ls /bin
mkdir /work
echo first line > /work/result
echo "second line, with  two  spaces" >> /work/result
cat /work/result
nosuchcommand
status
cat /does/not/exist
status
rm /work
exit 7
