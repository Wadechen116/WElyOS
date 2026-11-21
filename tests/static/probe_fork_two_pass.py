#!/usr/bin/env python3
"""Production fork_table two-pass checks with failure at each child walk."""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile
from probe_pte_ticket import function
from probe_fork_parent_failure import FIXTURE

ROOT=Path(__file__).resolve().parents[2]
HARNESS=r'''
int main(void){
 unsigned count=0;
 const unsigned states[]={VM_WRITE,VM_COW,VM_COW|VM_TICKET,VM_SHARED,VM_SHARED|VM_WRITE,0};
 for(unsigned pattern=0;pattern<1296;pattern++) for(int failure=0;failure<=4;failure++) {
  pte_t parent[4],before[4];unsigned x=pattern;
  for(unsigned i=0;i<4;i++){parent[i]=((i+1)*4096UL)|states[x%6];x/=6;}
  memcpy(before,parent,sizeof before);memset(child_ptes,0,sizeof child_ptes);
  struct vm_space child={0};struct vm_range range={0,8192};
  struct fork_anon fa={&range,1,0};walk_count=ref_count=0;fail_at=failure;
  int rc=fork_table(&child,parent,0,0,0,3,&fa,0);
  if(memcmp(parent,before,sizeof before) || fa.marked) return 10;
  if(failure<4) {
   if(rc!=-1 || child.pages!=(unsigned)failure) return 11;
  } else {
   if(rc!=0 || child.pages!=4) return 12;
   unsigned refs=0,marked=0;
   for(unsigned i=0;i<4;i++) {
    unsigned flags=before[i]&4095;
    if(!(flags&VM_SHARED))refs++;
    if(!(flags&VM_SHARED) && (flags&VM_WRITE)) {
     flags=(flags&~VM_WRITE)|VM_COW;
     if(i<2){flags|=VM_TICKET;marked++;}
    }
    if(child_ptes[i]!=((before[i]&~4095UL)|flags))return 13;
   }
   if(ref_count!=refs)return 14;
   unsigned calls=walk_count,refs_before=ref_count;
   if(fork_table(&child,parent,0,0,0,3,&fa,1)!=0)return 15;
   if(walk_count!=calls || ref_count!=refs_before || child.pages!=4 || fa.marked!=marked)return 16;
   if(memcmp(parent,child_ptes,sizeof parent))return 17;
  }
  count++;
 }
 printf("PASS %u mixed ANON/legacy/shared first-pass failures and two-pass success cases\n",count);
 return 0;
}
'''

if __name__=='__main__':
 src=(ROOT/'kernel/vm.c').read_text();out=Path(tempfile.mkdtemp(prefix='welyos-fork-two-pass-'))
 prefix=FIXTURE.split('CODE')[0].replace('#include <stdio.h>','#include <stdio.h>\n#include <string.h>')
 prefix=prefix.replace('static void pmm_ref(void *p){(void)p;}', 'static unsigned ref_count,walk_count; static int fail_at; static pte_t child_ptes[4];\nstatic void pmm_ref(void *p){(void)p;ref_count++;}')
 prefix=prefix.replace('(void)o;return NULL;', '(void)o;unsigned call=walk_count++;return (int)call==fail_at?NULL:&child_ptes[a/4096];')
 code='\n'.join(function(src,n) for n in ('in_anon','fork_table'));rows=[]
 for name,body in [('none',code),('early-parent-write',code.replace('if (mark_parent) {','if (1) {')),('second-pass-allocates',code.replace('if (mark_parent)\n            continue;','if (0)\n            continue;'))]:
  c=out/(name+'.c');c.write_text(prefix+body+HARNESS);exe=c.with_suffix('')
  subprocess.run(['cc','-O1','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],check=True,capture_output=True)
  p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
  ok=p.returncode==0 if name=='none' else p.returncode in range(10,18)
  rows.append(dict(case=name,passed=ok,exit=p.returncode,stdout=p.stdout,stderr=p.stderr));print(name,ok,p.stdout.strip(),p.returncode)
 changed=(ROOT/'kernel/vm.c').read_text()!=src
 (out/'results.json').write_text(json.dumps(dict(source_sha256=hashlib.sha256(src.encode()).hexdigest(),changed_input=changed,cases=rows),indent=2)+'\n')
 print('artifacts:',out,'changed_input:',changed)
 raise SystemExit(int(changed or not all(r['passed'] for r in rows)))
