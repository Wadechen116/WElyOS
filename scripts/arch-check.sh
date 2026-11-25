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
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
proc_scan() {   # $1 = 要檢查的根目錄;違規時印出 檔案:行 並回傳 1
  python3 - "$1" <<'PY'
import re, sys, glob, os
root = sys.argv[1]
bad = []
# Keep literals intact while removing comments, so strings containing /* are not comments.
lex = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\'')
def blanks(text):
    return ''.join('\n' if c == '\n' else ' ' for c in text)
def code(path):
    s = open(os.path.join(root, path), encoding='utf-8').read()
    return lex.sub(lambda m: blanks(m[0]) if m[0].startswith(('//', '/*')) else m[0], s)
def tree(*pats):
    return {os.path.relpath(f, root).replace(os.sep, '/')
            for p in pats for f in glob.glob(os.path.join(root, p), recursive=True)}
owner = {'sig': 'kernel/proc_signal.c', 'mem': 'kernel/proc_mem.c', 'files': 'kernel/proc_fs.c'}
core = {'kernel/proc.c', *owner.values()}
sources = sorted(tree('kernel/**/*.c', 'arch/**/*.c', 'lib/**/*.c'))
headers = sorted(tree('include/**/*.h', 'kernel/**/*.h', 'arch/**/*.h', 'lib/**/*.h'))
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
            # 用 m.start(1) 算行號:^\s* 在 re.M 下會跨過換行(前一行是被拿掉的註解時),m.start() 會早一行
            n = clean.count('\n', 0, m.start(1)) + 1
            bad.append(f"{path}:{n}: 不可以引入 {h}")
if bad:
    print("架構檢查:%d 處違反模組的界線" % len(bad))
    for b in bad:
        print("  " + b)
    sys.exit(1)
print("架構檢查:行程管理的模組界線沒有被跨過(%d 個原始檔)" % len(sources))
PY
}
# 先證明這條規則抓得到、行號正確:一個該失敗的樣本(kernel/ 底下的一般檔案引入 proc_internal.h,
# 前一行是註解;必須回報在第 2 行而不是第 1 行),一個該通過的樣本(kernel/proc.c 引入它)
mkdir -p "$tmp/proc-bad/kernel" "$tmp/proc-ok/kernel"
printf '%s\n' '/* #include "proc_internal.h" 註解不算 */' '#include "proc_internal.h"' > "$tmp/proc-bad/kernel/x.c"
printf '%s\n' '#include "proc_internal.h"' > "$tmp/proc-ok/kernel/proc.c"
if proc_scan "$tmp/proc-bad" > "$tmp/proc-bad.log" || ! grep -q '^  kernel/x.c:2:' "$tmp/proc-bad.log" \
   || grep -q 'kernel/x.c:1:' "$tmp/proc-bad.log"; then
  echo "  架構檢查:行程管理界線的規則沒有正確抓到該失敗的樣本"; cat "$tmp/proc-bad.log"; exit 1
fi
if ! proc_scan "$tmp/proc-ok" > "$tmp/proc-ok.log"; then
  echo "  架構檢查:行程管理界線的規則誤擋了 kernel/proc.c 的樣本"; cat "$tmp/proc-ok.log"; exit 1
fi
proc_scan . || exit 1

# ---- 編譯器的防線還在嗎 ----
# 上面是文字層次的檢查;真正擋住跨界存取的是編譯器(proc_internal.h 的 PSIG_F / PMEM_F / PFILES_F:
# 不是擁有者的檔案看不到欄位真正的名稱)。這裡實際編譯幾個小片段:
#   「應該失敗」的 — 沒有擁有者的記號卻存取那一組的欄位;以及系統呼叫層(只有 proc_calls.h)存取任何欄位
#   「應該成功」的 — 有記號的擁有者存取自己的欄位(確認失敗不是因為片段本身寫錯)
# 任何一個結果不符就失敗:表示有人把這道防線拿掉或繞過了。
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

# ---- 自我測試的診斷分類器只能留在測試程式裡 ----
# include/i349_classify.h 是自我測試用的純 C 診斷分類器;只有 kernel/test/ 與 tests/host/ 可以引入它。
# 其他原始檔或標頭檔(kernel/、arch/、lib/、user/、include/,不含 kernel/test/)引入它就失敗,
# 不論是 "i349_classify.h"、<i349_classify.h> 還是帶路徑的 ../include/i349_classify.h。
# 這個標頭檔還不存在時,規則照樣成立(沒有人引入它)。註解裡提到不算。
i349_scan() {   # $1 = 要檢查的根目錄;違規時印出 檔案:行 並回傳 1
  python3 - "$1" <<'PY'
import re, sys, glob, os
root = sys.argv[1]
lex = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\'')
def blanks(text):
    return ''.join('\n' if c == '\n' else ' ' for c in text)
def code(path):
    s = open(path, encoding='utf-8', errors='replace').read()
    return lex.sub(lambda m: blanks(m[0]) if m[0].startswith(('//', '/*')) else m[0], s)
files = set()
for d in ('kernel', 'arch', 'lib', 'user', 'include'):
    for ext in ('c', 'h'):
        files.update(glob.glob(os.path.join(root, d, '**', '*.' + ext), recursive=True))
rel = sorted(os.path.relpath(f, root).replace(os.sep, '/') for f in files)
rel = [p for p in rel if not p.startswith(('kernel/test/', 'tests/host/'))]
bad = []
for path in rel:
    clean = code(os.path.join(root, path))
    for m in re.finditer(r'^\s*#\s*include\s*[<"]([^>"\n]+)[>"]', clean, re.M):
        if m.group(1).rsplit('/', 1)[-1] == 'i349_classify.h':
            n = clean.count('\n', 0, m.start(1)) + 1
            bad.append(f"{path}:{n}: 不可以引入 i349_classify.h(只有 kernel/test/ 與 tests/host/ 可以)")
if bad:
    print("架構檢查:%d 處在測試程式以外引入了自我測試的診斷分類器" % len(bad))
    for b in bad:
        print("  " + b)
    sys.exit(1)
print("架構檢查:自我測試的診斷分類器只在測試程式中使用(%d 個原始檔)" % len(rel))
PY
}
# 先證明這條規則抓得到:一個該失敗的樣本(kernel/ 底下用帶路徑的角括號引入,前一行的註解不算),
# 一個該通過的樣本(kernel/test/ 底下引入)
mkdir -p "$tmp/i349-bad/kernel" "$tmp/i349-ok/kernel/test"
printf '%s\n' '// #include "i349_classify.h" 註解不算' '#include <../include/i349_classify.h>' > "$tmp/i349-bad/kernel/x.c"
printf '%s\n' '#include "../../include/i349_classify.h"' > "$tmp/i349-ok/kernel/test/x.c"
if i349_scan "$tmp/i349-bad" > "$tmp/i349-bad.log" || ! grep -q '^  kernel/x.c:2:' "$tmp/i349-bad.log" \
   || grep -q 'kernel/x.c:1:' "$tmp/i349-bad.log"; then
  echo "  架構檢查:診斷分類器的規則沒有正確抓到該失敗的樣本"; cat "$tmp/i349-bad.log"; exit 1
fi
if ! i349_scan "$tmp/i349-ok" > "$tmp/i349-ok.log"; then
  echo "  架構檢查:診斷分類器的規則誤擋了 kernel/test/ 的樣本"; cat "$tmp/i349-ok.log"; exit 1
fi
i349_scan . || exit 1
