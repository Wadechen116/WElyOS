#!/usr/bin/env python3
"""Isolated fork_start/frame KOBJ proposal with current real caller/VM helpers."""
from pathlib import Path
import hashlib,json,subprocess,tempfile
import probe_fork_caller_current as current
from probe_pte_ticket import function
ROOT=Path(__file__).resolve().parents[2]

def build(src):
    body=current.build(src)
    body=body.replace('struct fork_start {struct proc *p;void *frame;};',
                      'struct fork_start {struct mb_domain *dom;unsigned long kobj;struct proc *p;void *frame;};')
    body=body.replace('static size_t kmalloc_size(void *p){return ((size_t*)p)[-1];}',
                      'static unsigned round_extra,child_limit=1000,force_charge_ref,charge_calls,charge_seen,fail_root;\nstatic size_t kmalloc_size(void *p){return ((size_t*)p)[-1]+round_extra;}\nstatic unsigned long mb_pages_for_bytes(unsigned long n){return (n+PAGE_SIZE-1)/PAGE_SIZE;}')
    body=body.replace('{(void)p;return mb_domain_new(1000);}', '{(void)p;return mb_domain_new(child_limit);}')
    mb='\n'.join(function(src['kernel/membudget.c'],n) for n in ['mb_charge_object','mb_release_object'])
    mb=mb.replace('int mb_charge_object(', 'int actual_mb_charge_object(')
    mb+='''
static int mb_charge_object(struct mb_domain *d,unsigned long n){
 charge_calls++;unsigned refs=d->refs;if(force_charge_ref)d->refs=MB_REFS_MAX;
 int rc=actual_mb_charge_object(d,n);if(force_charge_ref)d->refs=refs;
 if(rc==0){charge_seen++;if(n!=2*(round_extra?2:1)||d->refs!=refs+1)panic("temporary charge size/ref");}
 return rc;
}
'''
    marker='static struct mb_domain *pmem_domain_new'
    body=body.replace(marker,mb+'\n'+marker)
    helper=function(src['kernel/proc.c'],'fork_start_release')
    body=body.replace(function(src['kernel/proc.c'],'fork_child_main'),helper+'\n'+function(src['kernel/proc.c'],'fork_child_main'))
    # Root allocation injection only occurs in the child's vm_space_create.
    body=body.replace('static pte_t *table_alloc(void){', 'static pte_t *table_alloc(void){if(fail_root){fail_root=0;return NULL;}')
    # Existing full caller controls: force thread reject, all six heap alloc sites,
    # normal/dying/killed consumer. Add quota/ref/root rejects after parent snapshot.
    body=body.replace('char tf[64]={0};if(fail>=2)',
                      'char tf[64]={0};if(fail==8)child_limit=(round_extra?2:1)+2*(round_extra?2:1)-1; if(fail==9)child_limit=(round_extra?2:1)+2*(round_extra?2:1);if(fail==10)force_charge_ref=1;if(fail==11)fail_root=1;if(fail>=2&&fail<=7)')
    body=body.replace('alloc_calls=alloc_fail=live=errors=started=resumed=exited=0;',
                      'child_limit=1000;force_charge_ref=charge_calls=charge_seen=fail_root=0;alloc_calls=alloc_fail=live=errors=started=resumed=exited=0;')
    body=body.replace('if(fail){\n  if(rc', 'if(fail){\n  if(fail>=8 && (charge_calls!=1||charge_seen!=(fail==9||fail==11)))return 40;\n  if(rc')
    body=body.replace('struct proc *c=procs;vm_space_destroy(c->space);',
                      'struct proc *c=procs;if(charge_calls!=1||charge_seen!=1||c->mem.dom->used[MB_KOBJ]!=(round_extra?2:1)||c->mem.dom->refs!=2)return 41;vm_space_destroy(c->space);')
    start=body.index('int main(int argc,char **argv)')
    body=body[:start]+r'''
int main(void){unsigned count=0;
 for(unsigned extra=0;extra<2;extra++)for(unsigned n=1;n<=4;n++){
  round_extra=extra*PAGE_SIZE;
  for(unsigned failure=1;failure<=11;failure++){start_dying=0;int rc=trial(n,failure,1);if(rc)return rc;count++;}
  for(unsigned mode=0;mode<3;mode++){start_dying=mode;int rc=trial(n,0,1);if(rc)return rc;count++;}
 }
 printf("PASS %u actual fork caller controls: normal/dying/killed, six heap sites, thread reject, temporary quota/ref reject, child-root quota/alloc reject, two rounding costs\n",count);return 0;
}
'''
    return body

def main():
    paths=current.PATHS+['kernel/proc_internal.h']
    src={p:(ROOT/p).read_text() for p in paths}
    out=Path(tempfile.mkdtemp(prefix='welyos-fork-tempobj-probe-'))
    for p,s in src.items():
        f=out/p;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(s)
    patch=ROOT/'tests/static/proposals/fork-tempobj-kobj-20261121.patch'
    cp=subprocess.run(['patch','-p1','--batch','--fuzz=0','-i',str(patch)],cwd=out,capture_output=True,text=True)
    if cp.returncode:raise RuntimeError(cp.stdout+cp.stderr)
    patched=dict(src);patched['kernel/proc.c']=(out/'kernel/proc.c').read_text()
    body=build(patched)
    variants={
      'baseline':(body,0),
      'omit-charge-release':(body.replace('mb_release_object(dom, kobj) != 0','0'),10),
      'omit-frame-free':(body.replace('kfree(st->frame);','/* mutation */'),10),
      'omit-start-free':(body.replace('kfree(st);\n    if (dom','/* mutation */\n    if (dom'),10),
      'omit-frame-cost':(body.replace(' + mb_pages_for_bytes(kmalloc_size(frame))',''),90),
    }
    rows=[]
    for name,(b,expected) in variants.items():
        if name!='baseline' and b==body:raise RuntimeError('mutation missed')
        c=out/(name+'.c');c.write_text(b);exe=c.with_suffix('')
        comp=subprocess.run(['cc','-std=c11','-O1','-g','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],capture_output=True,text=True)
        r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30) if comp.returncode==0 else comp
        rows.append(dict(variant=name,compile_exit=comp.returncode,exit=r.returncode,expected=expected,passed=comp.returncode==0 and r.returncode==expected,stdout=r.stdout,stderr=r.stderr));print(name,rows[-1]['passed'],r.returncode,r.stdout.splitlines()[-1:] or r.stderr.strip())
    changed=[p for p,s in src.items() if (ROOT/p).read_text()!=s]
    (out/'results.json').write_text(json.dumps(dict(inputs={p:hashlib.sha256(s.encode()).hexdigest() for p,s in src.items()},changed_inputs=changed,patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest(),cases=rows),indent=2)+'\n')
    print('artifacts:',out,'changed_inputs:',changed)
    return bool(changed) or not all(r['passed'] for r in rows)

if __name__=='__main__':raise SystemExit(main())
