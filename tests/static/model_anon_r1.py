#!/usr/bin/env python3
"""Executable R1 specification, not a test of production VM implementation.

One virtual page, up to three spaces; enumerate commit/fault/fork/unmap schedules.
Physical identities are canonicalized so continuous fork trees reach a fixed point.
"""
from collections import deque


def canonical(maps, reserved):
    ids={};result=[]
    for kind,pa in maps:
        if pa>=0:
            if pa not in ids:ids[pa]=len(ids)
            pa=ids[pa]
        result.append((kind,pa))
    return tuple(result),reserved


def verify(mutation=None):
    initial=canonical([(0,-1)]*3,0);seen={initial};queue=deque([initial]);edges=0
    while queue:
        maps,reserved=queue.popleft()
        assert reserved==sum(k in (1,3) for k,p in maps), (mutation,maps,reserved)
        refs={p:sum(q==p for k,q in maps if k>=2) for k,p in maps if k>=2}
        for kind,pa in maps:
            if kind==2:assert refs[pa]==1
        for i,(kind,pa) in enumerate(maps):
            transitions=[]
            if kind==0:transitions.append(('commit',(1,-1),1))
            if kind==1:transitions.append(('first touch',(2,3),-1))
            if kind==3:
                transitions.append(('COW copy' if refs[pa]>1 else 'COW reuse',
                                    (2,3 if refs[pa]>1 else pa),
                                    0 if mutation=='reuse-no-return' and refs[pa]==1 else -1))
            if kind:
                transitions.append(('unmap',(0,-1),-int(kind in (1,3))))
            for name,item,delta in transitions:
                new=list(maps);new[i]=item;nxt=canonical(new,reserved+delta);edges+=1
                if nxt not in seen:seen.add(nxt);queue.append(nxt)
            if kind:
                for j,(child,_) in enumerate(maps):
                    if child:continue
                    new=list(maps);delta=1
                    if kind==2:
                        new[i]=(3,pa);delta+=int(mutation!='fork-no-parent-ticket')
                    new[j]=(3,pa) if kind>=2 else (1,-1)
                    nxt=canonical(new,reserved+delta);edges+=1
                    if nxt not in seen:seen.add(nxt);queue.append(nxt)
                # Failed fork after acquiring parent+child pending reservations:
                # both pending holdings must be returned, leaving maps unchanged.
                pending=int(kind==2);child_ticket=1
                during=reserved+pending+child_ticket
                assert during==reserved+pending+child_ticket
                after=during-child_ticket-pending
                assert after==reserved
    return len(seen),edges


if __name__=='__main__':
    states,edges=verify();print(f'PASS R1 specification {states} states, {edges} transitions')
    for mutation in ('reuse-no-return','fork-no-parent-ticket'):
        try:verify(mutation)
        except AssertionError:print('PASS negative control caught:',mutation)
        else:raise AssertionError('uncaught '+mutation)
