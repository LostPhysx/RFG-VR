// For each string given (exact, or substring when prefixed with ~), finds its address(es) in the program, then every reference to it,
// and decompiles the referencing functions (deduplicated) into a file.
// Usage (headless): -postScript StringXrefs.java <out.txt> <string> [<string> ...]
// @category rfg-vr
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.HashSet;
import java.util.Set;

public class StringXrefs extends GhidraScript {
    private static final long MAX_SIZE = 20000;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: StringXrefs <out.txt> <string>...");
            return;
        }
        Set<String> wanted = new HashSet<>();
        for (int i = 1; i < args.length; i++) wanted.add(args[i]);

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        Set<Long> done = new HashSet<>();
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            DataIterator it = currentProgram.getListing().getDefinedData(true);
            while (it.hasNext() && !monitor.isCancelled()) {
                Data d = it.next();
                if (!d.hasStringValue()) continue;
                Object v = d.getValue();
                if (v == null) continue;
                String vs = v.toString();
                boolean hit = wanted.contains(vs);
                for (String w : wanted) if (w.startsWith("~") && vs.contains(w.substring(1))) hit = true;
                if (!hit) continue;
                Address sa = d.getAddress();
                out.println("######## string \"" + v + "\" @ " + sa);
                for (Reference ref : getReferencesTo(sa)) {
                    Address from = ref.getFromAddress();
                    Function f = getFunctionContaining(from);
                    out.println("# ref from " + from + " in " + (f != null ? f.getName() + "@" + f.getEntryPoint() : "(no function)"));
                    // Pointer tables: follow one more level (data referencing the string, referenced by code).
                    if (f == null) {
                        for (Reference r2 : getReferencesTo(from)) {
                            Function f2 = getFunctionContaining(r2.getFromAddress());
                            out.println("#   via table " + from + " <- " + r2.getFromAddress() + " in "
                                + (f2 != null ? f2.getName() + "@" + f2.getEntryPoint() : "(no function)"));
                            if (f2 != null) f = f2;
                        }
                    }
                    if (f == null || !done.add(f.getEntryPoint().getOffset())) continue;
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
                        out.println("// (too large to decompile)");
                        continue;
                    }
                    DecompileResults r = decomp.decompileFunction(f, 120, monitor);
                    out.println(r != null && r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed");
                    out.flush();
                }
            }
        } finally {
            decomp.dispose();
        }
        println("StringXrefs done");
    }
}
