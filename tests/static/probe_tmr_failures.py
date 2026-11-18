import pathlib,tempfile,subprocess,os,signal,json
root=pathlib.Path.cwd(); out=pathlib.Path(tempfile.mkdtemp(prefix='welyos-tmr-107-'))
(out/'tmr.h').write_bytes((root/'user/tmr.h').read_bytes())
(out/'libc.h').write_text('''#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdio.h>
static int nth, fail_at;
static long sys_fork(void) { if (++nth == fail_at) return -1; return fork(); }
#define sys_pipe pipe
#define sys_close close
#define sys_kill kill
#define sys_dup2 dup2
#define sys_read read
#define sys_write write
#define sys_exit _exit
static long sys_waitpid(long p,int *s,int f) { return waitpid(p,s,f); }
static void sys_sleep(long ms) { usleep(ms*1000); }
''')
(out/'probe.c').write_text('''#include "tmr.h"
static int work(void *arg) { if (fail_at==4) for (;;) pause(); return 0; }
int main(int argc,char **argv) { fail_at=atoi(argv[1]); struct tmr_result r[3]; struct tmr_report p;
int rc=tmr_run(work,0,20,0,0,r,&p); printf("rc=%d\\n",rc); return 0; }
''')
subprocess.run(['gcc','-O0','-o',str(out/'probe'),str(out/'probe.c')],check=True)
results=[]
for n in (0,1,4):
 p=subprocess.Popen([str(out/'probe'),str(n)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True)
 try:
  stdout,stderr=p.communicate(timeout=1)
  results.append({'fork_failure_at':n,'timeout':False,'returncode':p.returncode,'stdout':stdout.decode()})
 except subprocess.TimeoutExpired:
  os.killpg(p.pid,signal.SIGKILL);p.communicate()
  results.append({'fork_failure_at':n,'timeout':True})
(out/'results.json').write_text(json.dumps(results,indent=2));print(out);print(json.dumps(results,indent=2))
