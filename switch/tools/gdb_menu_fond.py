# Traçage gdb du choix du fond du menu principal (PC) : gdb -batch -ex 'source switch/tools/gdb_menu_fond.py' -ex run --args ...
# (voir CLAUDE.md, « Fond du menu principal »). Mémoire invitée lue à l'adresse hôte 0x100000000 + adresse invitée.
import gdb
BASE = 0x100000000
def u32(a):
    return int(gdb.parse_and_eval('*(unsigned int*)%d' % a)) & 0xffffffff
def be32(g):
    v = u32(BASE + g)
    return int.from_bytes(v.to_bytes(4, 'little'), 'big')
def wstr(g, n=64):
    out = ''
    inf = gdb.selected_inferior()
    m = bytes(inf.read_memory(BASE + g, n * 2))
    for i in range(0, len(m), 2):
        c = int.from_bytes(m[i:i+2], 'big')
        if c == 0: break
        out += chr(c)
    return out
class Fin(gdb.FinishBreakpoint):
    def __init__(self, ctx):
        super().__init__(gdb.newest_frame(), internal=True)
        self.ctx = ctx
    def stop(self):
        r3 = u32(self.ctx) 
        print('[menu] sub_822E7AE0 -> r3=%08x' % r3)
        if r3:
            try:
                ptr = be32(r3 + 2084); size = be32(r3 + 2084 + 16)
                print('[menu]   +2084 taille', size, repr(wstr(ptr, size) if size >= 8 else wstr(r3 + 2084, size)))
            except Exception as e: print('[menu] lecture', e)
        return False
class Ent(gdb.Breakpoint):
    def stop(self):
        ctx = int(gdb.parse_and_eval('$rdi'))
        r3 = u32(ctx); r4 = u32(ctx + 32); lr = u32(ctx + 256)
        if lr != 0x82217324: return False
        print('[menu] sub_822E7AE0(r3=%08x, r4=%08x)' % (r3, r4))
        Fin(ctx)
        return False
Ent('__imp__sub_822E7AE0')
class Menu(gdb.Breakpoint):
    def stop(self):
        ctx = int(gdb.parse_and_eval('$rdi'))
        print('[menu] sub_82217248(r3=%08x, r4=%08x) lr=%08x' % (u32(ctx), u32(ctx + 32), u32(ctx + 256)))
        return False
Menu('__imp__sub_82217248')
