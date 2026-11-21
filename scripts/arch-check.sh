#!/usr/bin/env bash
# 架構規則的檢查:模組之間的界線有沒有被跨過(回歸測試的一部分,不到一秒)。
#
# 行程管理(第 47 步)把狀態依擁有者分成四組,規則如下;違反任何一條就失敗:
#   1. p->sig.    只能出現在 kernel/proc_signal.c
#      p->mem.    只能出現在 kernel/proc_mem.c
#      p->files.  只能出現在 kernel/proc_fs.c
#   2. kernel/syscall.c 不可以引入 proc_internal.h(它只能看到不透明的 struct proc)
#   3. proc_internal.h 只能被 kernel/proc*.c 引入;proc_calls.h 只能被它與 kernel/syscall.c 引入
#      (kernel 的其他部分用的是 include/proc.h)
# 第 2 條其實由編譯器保證(看不到定義就存取不了欄位);這裡檢查的是「沒有人把那個引入加回去」。
# 註解裡提到這些寫法不算(檢查前先把註解拿掉)。
cd "$(dirname "$0")/.."
python3 - <<'PY'
import re, sys, glob
bad = []
# Keep literals intact while removing comments, so strings containing /* are not comments.
lex = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\'')
def blanks(text):
    return ''.join('\n' if c == '\n' else ' ' for c in text)
def code(path):
    s = open(path, encoding='utf-8').read()
    return lex.sub(lambda m: blanks(m[0]) if m[0].startswith(('//', '/*')) else m[0], s)
owner = {'sig': 'kernel/proc_signal.c', 'mem': 'kernel/proc_mem.c', 'files': 'kernel/proc_fs.c'}
core = {'kernel/proc.c', *owner.values()}
sources = sorted(set(glob.glob('kernel/**/*.c', recursive=True) +
                     glob.glob('arch/**/*.c', recursive=True) + glob.glob('lib/**/*.c', recursive=True)))
headers = sorted(set(glob.glob('include/**/*.h', recursive=True) +
                     glob.glob('kernel/**/*.h', recursive=True) + glob.glob('arch/**/*.h', recursive=True) +
                     glob.glob('lib/**/*.h', recursive=True)))
for path in sources + headers:
    clean = code(path)
    # Do not match documentation strings or include strings as field access.
    fields = lex.sub(lambda m: blanks(m[0]), clean)
    for m in re.finditer(r'(?:->|\.)\s*(sig|mem|files)\b', fields):
        grp = m.group(1)
        if path != owner[grp]:
            n = fields.count('\n', 0, m.start()) + 1
            bad.append(f"{path}:{n}: 存取了 {grp} 這一組的狀態(只有 {owner[grp]} 可以)")
    for m in re.finditer(r'^\s*#\s*include\s*[<"]([^>"\n]+)[>"]', clean, re.M):
        h = m.group(1).rsplit('/', 1)[-1]
        if h not in ('proc_internal.h', 'proc_calls.h'):
            continue
        ok = path in core if h == 'proc_internal.h' else path in ('kernel/proc_internal.h', 'kernel/syscall.c')
        if not ok:
            n = clean.count('\n', 0, m.start()) + 1
            bad.append(f"{path}:{n}: 不可以引入 {h}")
if bad:
    print("架構檢查:%d 處違反模組的界線" % len(bad))
    for b in bad:
        print("  " + b)
    sys.exit(1)
print("架構檢查:行程管理的模組界線沒有被跨過(%d 個原始檔)" % len(sources))
PY

# ---- 編譯器的防線還在嗎 ----
# 上面是文字層次的檢查;真正擋住跨界存取的是編譯器(proc_internal.h 的 PSIG_F / PMEM_F / PFILES_F:
# 不是擁有者的檔案看不到欄位真正的名稱)。這裡實際編譯幾個小片段:
#   「應該失敗」的 — 沒有擁有者的記號卻存取那一組的欄位;以及系統呼叫層(只有 proc_calls.h)存取任何欄位
#   「應該成功」的 — 有記號的擁有者存取自己的欄位(確認失敗不是因為片段本身寫錯)
# 任何一個結果不符就失敗:表示有人把這道防線拿掉或繞過了。
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc_try() {   # $1 = 檔名;回傳編譯器的結束碼
  gcc -m32 -std=c11 -ffreestanding -fsyntax-only -Iinclude -Ikernel -Iarch/i386 "$tmp/$1" > "$tmp/$1.log" 2>&1
}
probe() {    # $1 = 名稱, $2 = 預期(ok / fail), 其餘 = 原始碼
  name=$1; want=$2; shift 2
  printf '%s\n' "$@" > "$tmp/$name.c"
  if cc_try "$name.c"; then got=ok; else got=fail; fi
  if [ "$got" != "$want" ]; then
    echo "  架構檢查:編譯片段 $name 預期 $want,實際 $got"; bad=1
  fi
}
bad=0
if ! command -v gcc >/dev/null; then
  echo "  架構檢查:找不到 gcc,無法確認編譯器的防線"; exit 2
fi
probe owner-sig   ok   '#define PROC_OWNER_SIG'   '#include "proc_internal.h"' 'int f(struct proc *p) { return p->sig.stopped; }'
probe owner-mem   ok   '#define PROC_OWNER_MEM'   '#include "proc_internal.h"' 'uintptr_t f(struct proc *p) { return p->mem.brk; }'
probe owner-files ok   '#define PROC_OWNER_FILES' '#include "proc_internal.h"' 'void *f(struct proc *p) { return p->files.fd[0]; }'
probe other-sig   fail '#include "proc_internal.h"' 'int f(struct proc *p) { return p->sig.stopped; }'
probe other-sig2  fail '#define PROC_OWNER_MEM' '#include "proc_internal.h"' 'int f(struct proc *target) { return target -> sig . pending; }'
probe other-mem   fail '#define PROC_OWNER_SIG' '#include "proc_internal.h"' 'unsigned long f(struct proc *q) { return q->mem.brk; }'
probe other-files fail '#include "proc_internal.h"' 'void f(struct proc *p) { p->files.cwd[0] = 0; }'
probe core-field  ok   '#include "proc_internal.h"' 'int f(struct proc *p) { return p->pid; }'
probe syscall-any fail '#include "proc_calls.h"' 'int f(struct proc *p) { return p->pid; }'
probe syscall-api ok   '#include "proc_calls.h"' 'int f(struct proc *p) { return proc_pid(p); }'
if [ "$bad" -ne 0 ]; then exit 1; fi
echo "架構檢查:編譯器的防線有效(10 個片段:該失敗的 5 個都編譯不過,該成功的 5 個都通過)"

