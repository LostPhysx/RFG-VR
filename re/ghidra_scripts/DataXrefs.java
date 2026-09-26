// Lists every instruction that references the given data addresses and decompiles the
// referencing functions (deduplicated, size-capped) into a file.
// Usage (headless): -postScript DataXrefs.java <out.txt> <hexaddr> [<hexaddr> ...]
// @category rfg-vr
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

public class DataXrefs extends GhidraScript {
    private static final long MAX_SIZE = 6000;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DataXrefs <out.txt> <hexaddr>...");
            return;
        }
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        Set<Function> funcs = new LinkedHashSet<>();
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; i++) {
                Address a = toAddr(Long.parseLong(args[i].replaceFirst("^0[xX]", ""), 16));
                out.println("######## data " + a);
                for (Reference ref : getReferencesTo(a)) {
                    Address from = ref.getFromAddress();
                    Function f = getFunctionContaining(from);
                    out.println("# " + ref.getReferenceType() + " from " + from + "  " + getInstructionAt(from) + "   in "
                        + (f != null ? f.getName() + "@" + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses() : "?"));
                    if (f != null) funcs.add(f);
                }
            }
            out.println();
            for (Function f : funcs) {
                long size = f.getBody().getNumAddresses();
                out.println("//// " + f.getName() + " @ " + f.getEntryPoint() + " size=" + size);
                StringBuilder callers = new StringBuilder();
                int n = 0;
                for (Reference cr : getReferencesTo(f.getEntryPoint())) {
                    Function c = getFunctionContaining(cr.getFromAddress());
                    callers.append(c != null ? c.getName() + "@" + c.getEntryPoint() : "?@" + cr.getFromAddress()).append(' ');
                    if (++n >= 20) break;
                }
                out.println("// callers: " + callers);
                if (size > MAX_SIZE) {
                    out.println("// (too large to decompile)\n");
                    continue;
                }
                DecompileResults r = decomp.decompileFunction(f, 120, monitor);
                out.println(r != null && r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed");
                out.flush();
            }
        } finally {
            decomp.dispose();
        }
        println("DataXrefs done");
    }
}
