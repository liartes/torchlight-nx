# Traçage gdb (PC) du fichier lu par sub_8221DC20 pour le fond du menu (voir CLAUDE.md, « Fond du menu principal »).
import gdb, time
T0 = time.time()
BASE = 0x100000000
inf = None
def u32(a): return int(gdb.parse_and_eval('*(unsigned int*)%d' % a)) & 0xffffffff
def be32(g):
    return int.from_bytes(bytes(gdb.selected_inferior().read_memory(BASE + g, 4)), 'big')
def wstr(g, n=128):
    m = bytes(gdb.selected_inferior().read_memory(BASE + g, n * 2)); out = ''
    for i in range(0, len(m), 2):
        c = int.from_bytes(m[i:i+2], 'big')
        if c == 0: break
        out += chr(c)
    return out
def cstr(g, n=32):
    m = bytes(gdb.selected_inferior().read_memory(BASE + g, n)); return m.split(b'\0')[0]
def stdw(g):  # std::wstring MSVC : tampon interne si capacité < 8
    cap = be32(g + 20); size = be32(g + 16)
    return wstr(be32(g) if cap >= 8 else g, size)
class Fin(gdb.FinishBreakpoint):
    def __init__(self, ctx, out):
        super().__init__(gdb.newest_frame(), internal=True); self.ctx = ctx; self.out = out
    def stop(self):
        print('[menu]   -> FILE* %08x' % be32(self.out)); return False
class Open(gdb.Breakpoint):
    def stop(self):
        ctx = int(gdb.parse_and_eval('$rdi'))
        lr = u32(ctx + 256)
        if lr != 0x8221DC6C: return False
        r3, r4, r5 = u32(ctx), u32(ctx + 32), u32(ctx + 40)
        print('[menu] %.1f ouverture %r mode %r' % (time.time() - T0, wstr(r4), cstr(r5)))
        Fin(ctx, r3); return False
Open('__imp__sub_823A22A8')
class Dc(gdb.Breakpoint):
    def stop(self):
        ctx = int(gdb.parse_and_eval('$rdi'))
        print('[menu] %.1f sub_8221DC20 lr=%08x r4=%08x' % (time.time() - T0, u32(ctx + 256), u32(ctx + 32)))
        try: print('[menu]   nom', repr(stdw(u32(ctx + 32))))
        except Exception as e: print('[menu]', e)
        return False
Dc('__imp__sub_8221DC20')
