// Verifies the community (Sledge / RSL1) Steam addresses against our analyzed rfg.exe and
// dumps the rl_camera vtable recovered from RTTI. Output goes to the headless log.
// @category rfg-vr
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class VerifyHookMap extends GhidraScript {

    private static final Object[][] FUNCTIONS = {
        {"keen::graphics::beginFrame", 0xC6ABD0L},
        {"keen::graphics::endFrame", 0xC70320L},
        {"set_hud_hidden", 0x841A90L},
        {"set_fog_enabled", 0x7C2C70L},
        {"gameseq_get_state", 0x7BFCF0L},
        {"get_local_player", 0xA108D0L},
        {"human_teleport_unsafe", 0xA7C660L},
        {"human_get_head_pos_orient (RSL1 old RVA)", 0x400000L + 0x69B5D0L},
        {"rl_camera::render_begin (RSL1 old RVA)", 0x400000L + 0x137660L},
    };

    private static final Object[][] GLOBALS = {
        {"rfg_camera", 0x01DE4B50L},
        {"g_multiplayer", 0x02FEB588L},
        {"g_player_input_disabled", 0x01E2A9B9L},
        {"g_mouse_visible", 0x01CE86EAL},
    };

    @Override
    protected void run() throws Exception {
        println("=== image base " + currentProgram.getImageBase());
        println("=== functions");
        for (Object[] f : FUNCTIONS) {
            Address a = toAddr((Long) f[1]);
            Function fn = getFunctionAt(a);
            Function containing = getFunctionContaining(a);
            String status = fn != null ? "FUNCTION START" : containing != null
                ? "inside " + containing.getName() + " @" + containing.getEntryPoint() : "no function";
            println(String.format("%-42s %s  %s  size=%s  xrefs=%d  first: %s", f[0], a, status,
                fn != null ? fn.getBody().getNumAddresses() : "-", getReferencesTo(a).length, firstInsns(a, 4)));
        }
        println("=== globals");
        for (Object[] g : GLOBALS) {
            Address a = toAddr((Long) g[1]);
            Reference[] refs = getReferencesTo(a);
            StringBuilder users = new StringBuilder();
            for (int i = 0; i < Math.min(refs.length, 6); i++) {
                Function fn = getFunctionContaining(refs[i].getFromAddress());
                users.append(fn != null ? fn.getEntryPoint().toString() : refs[i].getFromAddress().toString()).append(' ');
            }
            println(String.format("%-26s %s  xrefs=%d  from: %s", g[0], a, refs.length, users));
        }
        println("=== rl_camera RTTI / vtable");
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Symbol s = it.next();
            String full = s.getName(true);
            if (!full.contains("rl_camera") || full.contains("rl_camera_")) continue;
            println("symbol " + full + " @ " + s.getAddress());
            if (s.getName().contains("vftable")) {
                Address p = s.getAddress();
                for (int i = 0; i < 40; i++) {
                    long target = getInt(p.add(i * 4L)) & 0xffffffffL;
                    Function fn = getFunctionAt(toAddr(target));
                    if (fn == null) break;
                    println(String.format("  vtbl[%2d] +0x%02X -> %08X %s (size %d)", i, i * 4, target, fn.getName(),
                        fn.getBody().getNumAddresses()));
                }
            }
        }
    }

    private String firstInsns(Address a, int n) {
        StringBuilder sb = new StringBuilder();
        Instruction ins = getInstructionAt(a);
        for (int i = 0; i < n && ins != null; i++, ins = ins.getNext()) sb.append(ins).append(" | ");
        return sb.toString();
    }
}
