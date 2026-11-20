# N-005 shell features, run by the kernel self-test (sh /etc/n005.sh).
# The kernel checks the exit code and the exact contents of /n5/out.
mkdir /n5
echo x > /n5/a.txt
echo x > /n5/b.txt
echo y > /n5/c.dat
NAME='hello world'
# quoted variable = one argument (args exits with argc: 2); unquoted = split into two (3)
args "$NAME" > /n5/o1
echo $? > /n5/out
args $NAME > /n5/o2
echo $? >> /n5/out
# quoted operators and an empty string are plain arguments
echo '|' '' "x" >> /n5/out
# globbing: sorted matches; quoted pattern stays literal; no match stays literal
echo /n5/*.txt >> /n5/out
echo '/n5/*.txt' >> /n5/out
echo /n5/*.none >> /n5/out
echo ${NAME}! >> /n5/out
cat /n5/?.dat >> /n5/out
# a redirection target that matches two files is refused (status 2), nothing is written
echo hi > /n5/*.txt
echo $? >> /n5/out
cat /n5/a.txt >> /n5/out
exit 5
