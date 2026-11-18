# Shell script run by the kernel self-test (sh /etc/test.sh).
# The kernel checks the exit code and the file this script leaves behind.
echo "  [sh] script started"
ls /bin
ls -l /etc
date
date +8
mkdir /work
# users and permissions
id
echo private > /work/p.txt
chmod 600 /work/p.txt
su alice id
su alice cat /work/p.txt
su alice rm /work/p.txt
su alice su root
ls -l /work
rm /work/p.txt
echo first line > /work/result
echo "second line, with  two  spaces" >> /work/result
cat /work/result
# working directory, relative paths, rename
cd /work
pwd
echo moved > old.txt
mv old.txt new.txt
ls
cat new.txt
rm new.txt
cd /nosuchdir
cd
# pipes and input redirection
ls /bin | wc
cat /etc/motd | cat | wc >> /work/result
wc < /etc/motd
ls /bin | cat > /work/names
echo pear >> /work/names
echo apple >> /work/names
sort /work/names
sort -r < /work/names | wc
rm /work/names
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
