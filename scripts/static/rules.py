"""有界、可解釋的 C 詞法檢查;不是預處理器、型別分析器或完整 C parser。"""
from dataclasses import dataclass
import hashlib
import re

LEX = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\[\s\S]|[^"\\])*"|'
                 r"'(?:\\[\s\S]|[^'\\])*'|[A-Za-z_]\w*|\d[\w.]*|"
                 r'->|<=|>=|==|!=|\+\+|--|&&|\|\||<<|>>|\+=|-=|[^\s]')
IDENT = re.compile(r'^[A-Za-z_]\w*$')
CONTROL = {'if', 'for', 'while', 'switch', 'sizeof', '_Alignof', 'return',
           '__attribute__', 'typeof', '_Static_assert'}
TYPES = {'void', 'char', 'short', 'int', 'long', 'unsigned', 'signed', 'const',
         'volatile', 'struct', 'enum', 'union', 'restrict', 'static'}
BANNED = {'gets', 'strcpy', 'strcat', 'sprintf', 'vsprintf', 'scanf', 'sscanf',
          'vscanf', 'vsscanf', 'alloca', '__builtin_alloca'}
ALLOC = {'kmalloc', 'pmm_alloc', 'pmm_alloc_page', 'pmm_alloc_pages'}


@dataclass
class Token:
    text: str
    line: int


@dataclass
class Function:
    path: str
    name: str
    line: int
    end: int
    params: list
    body: list
    pairs: dict
    calls: list
    digest: str

    @property
    def key(self):
        return f'{self.path}:{self.name}:{self.line}'


def tokens(source):
    # 移除預處理指令但保留行數;條件區塊仍全部分析,因此不是實際 build 可達性證明。
    source = re.sub(r'^\s*#(?:[^\n\\]|\\[^\n]|\\\n)*',
                    lambda m: '\n' * m[0].count('\n'), source, flags=re.M)
    out, line, previous = [], 1, 0
    for match in LEX.finditer(source):
        line += source[previous:match.start()].count('\n')
        text = match[0]
        if not text.startswith(('/*', '//')):
            out.append(Token(text, line))
        line += text.count('\n')
        previous = match.end()
    return out


def pair_tokens(ts):
    pairs, stack = {}, []
    for i, tok in enumerate(ts):
        if tok.text in ('(', '{', '['):
            stack.append(i)
        elif tok.text in (')', '}', ']'):
            expected = {')': '(', '}': '{', ']': '['}[tok.text]
            if stack and ts[stack[-1]].text == expected:
                start = stack.pop()
                pairs[start] = i
                pairs[i] = start
    return pairs


def functions(path, source):
    ts = tokens(source)
    pairs = pair_tokens(ts)
    result, i = [], 0
    while i < len(ts):
        if ts[i].text != '{' or i not in pairs:
            i += 1
            continue
        end = pairs[i]
        # 最外層的括號配對排除 struct/array initializer;一般函式以 name(...) { 開頭。
        if i >= 3 and ts[i-1].text == ')' and i-1 in pairs:
            opening = pairs[i-1]
            name = ts[opening-1].text if opening else ''
            if IDENT.match(name) and name not in CONTROL:
                body = ts[i+1:end]
                bpairs = pair_tokens(body)
                calls = [(j, t.text) for j, t in enumerate(body[:-1])
                         if IDENT.match(t.text) and t.text not in CONTROL
                         and body[j+1].text == '(' and j+1 in bpairs]
                normalized = ' '.join(t.text for t in ts[opening-1:end+1])
                result.append(Function(path, name, ts[opening-1].line, ts[end].line,
                                       ts[opening+1:i-1], body, bpairs, calls,
                                       hashlib.sha256(normalized.encode()).hexdigest()))
        i = end + 1
    return result


def finding(fn, rule, category, message, line=None, detail=''):
    # 綁定函式 token 雜湊:前面插入行不會使 baseline 失效,函式語意改動則重新審核。
    identity = f'{rule}|{fn.path}|{fn.name}|{fn.digest}|{detail}'
    return {'id': hashlib.sha256(identity.encode()).hexdigest()[:24],
            'rule': rule, 'category': category, 'path': fn.path,
            'line': line or fn.line, 'symbol': fn.name, 'message': message,
            'function_sha256': fn.digest}


def check(sources):
    fs = [f for path, text in sorted(sources.items()) for f in functions(path, text)]
    output, by_name = [], {}
    for fn in fs:
        by_name.setdefault(fn.name, []).append(fn)

    def resolve(fn, name):
        candidates = by_name.get(name, [])
        # 使用者 syscall wrapper 與核心 sys_* 同名,不能因此把兩個位址空間接在一起。
        user = fn.path.startswith('user/')
        candidates = [c for c in candidates if c.path.startswith('lib/') or
                      c.path.startswith('user/') == user]
        if fn.path.startswith('arch/'):
            target = fn.path.split('/')[1]
            candidates = [c for c in candidates if not c.path.startswith('arch/') or
                          c.path.split('/')[1] == target]
        local = [c for c in candidates if c.path == fn.path]
        return local or candidates

    graph = {fn.key: set() for fn in fs}
    lookup = {fn.key: fn for fn in fs}
    error_names = {f.name for f in fs if any(
        f.body[i].text == 'return' and f.body[i+1].text == '-' and
        f.body[i+2].text.startswith('E_') for i in range(len(f.body)-2))}
    # 沿著 return callee(...) 傳播已知的 -E_* 契約;不猜測所有 int 都是錯誤碼。
    for _ in range(len(fs) + 1):
        before = len(error_names)
        for fn in fs:
            for j, name in fn.calls:
                if name not in error_names or not j:
                    continue
                if fn.body[j-1].text == 'return':
                    error_names.add(fn.name)
                elif j >= 2 and fn.body[j-1].text == '=':
                    var = fn.body[j-2].text
                    if any(fn.body[k].text == 'return' and fn.body[k+1].text == var
                           for k in range(len(fn.body)-1)):
                        error_names.add(fn.name)
        if len(error_names) == before:
            break

    for fn in fs:
        if fn.end - fn.line + 1 > 80:
            output.append(finding(fn, 'function-length', 'style',
                                  f'{fn.end-fn.line+1} physical lines (limit 80)'))
        params = {t.text for t in fn.params if IDENT.match(t.text)
                  and t.text not in TYPES and not t.text.endswith('_t')}
        assertions = any(name in {'assert', 'ASSERT', 'KASSERT', 'CHECK', '_Static_assert'}
                         for _, name in fn.calls) or any(t.text == '_Static_assert' for t in fn.body)
        guards = False
        for i, t in enumerate(fn.body[:-1]):
            if t.text == 'if' and fn.body[i+1].text == '(' and i+1 in fn.pairs:
                cond = fn.body[i+2:fn.pairs[i+1]]
                guards |= any(x.text in params for x in cond)
        if not assertions and not guards:
            output.append(finding(fn, 'missing-check', 'style',
                                  'no lexical assertion or if-condition referencing a parameter; '
                                  'caller preconditions may justify this'))
        for j, name in fn.calls:
            for callee in resolve(fn, name):
                graph[fn.key].add(callee.key)
            if name in BANNED:
                output.append(finding(fn, 'banned-call', 'candidate', f'call to {name}',
                                      fn.body[j].line, str(j)))
            close = fn.pairs[j+1]
            if name in error_names and close+1 < len(fn.body) and fn.body[close+1].text == ';':
                start = j
                while start and fn.body[start-1].text not in {';', '{', '}'}:
                    previous = start - 1
                    if fn.body[previous].text == ')' and previous in fn.pairs:
                        opening = fn.pairs[previous]
                        # 控制條件中的賦值不是 body 呼叫的結果。例:
                        # while ((n = read(...)) > 0) write(...);
                        if opening and fn.body[opening-1].text in {'if', 'while', 'for', 'switch'}:
                            break
                    start -= 1
                prefix = {t.text for t in fn.body[start:j]}
                if not prefix.intersection({'=', 'return', '+=', '-='}):
                    output.append(finding(fn, 'unchecked-error', 'candidate',
                                          f'discarded result of inferred -E_* function {name} '
                                          '(including explicit void discard)', fn.body[j].line, str(j)))
        if fn.path.startswith('kernel/net/'):
            for j, tok in enumerate(fn.body[:-1]):
                if tok.text not in {'for', 'while', 'do'}:
                    continue
                if tok.text == 'do':
                    output.append(finding(fn, 'loop-bound', 'candidate',
                                          'do-loop requires manual bound review', tok.line, str(j)))
                    continue
                if fn.body[j+1].text != '(' or j+1 not in fn.pairs:
                    continue
                condition = [t.text for t in fn.body[j+2:fn.pairs[j+1]]]
                if tok.text == 'for':
                    parts = ' '.join(condition).split(';')
                    condition = parts[1].split() if len(parts) == 3 else []
                if not any(t in {'<', '<=', '>', '>=', '!='} for t in condition):
                    output.append(finding(fn, 'loop-bound', 'candidate',
                                          'no visible comparison bound in loop condition', tok.line, str(j)))

    # Tarjan SCC:只報核心/架構中的直接呼叫 cycle。callback、巨集和組語不在證明範圍。
    core = {f.key for f in fs if f.path.startswith(('kernel/', 'arch/'))}
    indices, low, stack, active, counter = {}, {}, [], set(), [0]
    target_core = set()
    target = ''
    def visit(key):
        indices[key] = low[key] = counter[0]
        counter[0] += 1
        stack.append(key)
        active.add(key)
        for dst in sorted(graph[key] & target_core):
            if dst not in indices:
                visit(dst)
                low[key] = min(low[key], low[dst])
            elif dst in active:
                low[key] = min(low[key], indices[dst])
        if low[key] == indices[key]:
            component = []
            while True:
                dst = stack.pop()
                active.remove(dst)
                component.append(dst)
                if dst == key:
                    break
            if len(component) > 1 or key in graph[key]:
                members = sorted(component)
                output.append(finding(lookup[members[0]], 'recursion', 'candidate',
                                      f'{target}: possible direct call cycle: ' + ' -> '.join(members),
                                      detail=target + '|' + '|'.join(lookup[x].digest for x in members)))
    for target in ('i386', 'riscv64', 'aarch64'):
        target_core = {k for k in core if not lookup[k].path.startswith('arch/') or
                       lookup[k].path.split('/')[1] == target}
        indices, low, stack, active, counter = {}, {}, [], set(), [0]
        for key in sorted(target_core):
            if key not in indices:
                visit(key)
    marked = {p for p, s in sources.items() if '/* no-alloc-after-init */' in s}
    for fn in fs:
        if fn.path not in marked or fn.name.endswith('_init') or fn.name.startswith('init_'):
            continue
        pending, seen = [fn.key], set()
        while pending:
            key = pending.pop()
            if key in seen:
                continue
            seen.add(key)
            for _, name in lookup[key].calls:
                if name in ALLOC or name.startswith('pmm_alloc'):
                    output.append(finding(fn, 'allocation-after-init', 'candidate',
                                          f'marked file: direct-call reachability to {name} via {key}'))
            pending.extend(sorted(graph[key] - seen))
    metrics = {'source_files': len(sources), 'functions': len(fs),
               'call_edges': sum(len(v) for v in graph.values()),
               'error_code_functions_inferred': len(error_names),
               'no_alloc_files': sorted(marked), 'loop_bound_scope': ['kernel/net/']}
    # 同一個路徑可由不同 call chain 抵達;避免重複 ID。
    return sorted({f['id']: f for f in output}.values(),
                  key=lambda f: (f['path'], f['line'], f['rule'], f['id'])), metrics
