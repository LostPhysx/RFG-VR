// Decompiles the functions given as script arguments into a text file.
// Usage (headless): -postScript DumpFuncs.java <out.txt> <hexaddr> [<hexaddr> ...]
// Each function is written with its callers and callees so the call graph can be followed.
// @category rfg-vr
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.FileWriter;
import java.io.PrintWriter;

public class DumpFuncs extends GhidraScript {

    private DecompInterface decomp;
    private PrintWriter out;

    private String callersOf(Function f) {
        StringBuilder sb = new StringBuilder();
        int n = 0;
        for (Reference ref : getReferencesTo(f.getEntryPoint())) {
            Function c = getFunctionContaining(ref.getFromAddress());
            sb.append(c != null ? c.getName() + "@" + c.getEntryPoint() : "?@" + ref.getFromAddress()).append(' ');
            if (++n >= 30) { sb.append("..."); break; }
        }
        return sb.toString();
    }

    private String calleesOf(Function f) throws Exception {
        StringBuilder sb = new StringBuilder();
        for (Function c : f.getCalledFunctions(monitor)) sb.append(c.getName()).append(' ');
        return sb.toString();
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DumpFuncs <out.txt> <hexaddr>...");
            return;
        }
        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        out = new PrintWriter(new FileWriter(args[0]));
        try {
            for (int i = 1; i < args.length; i++) {
                long a = Long.parseLong(args[i].replaceFirst("^0[xX]", ""), 16);
                Function f = getFunctionAt(toAddr(a));
                if (f == null) f = getFunctionContaining(toAddr(a));
                if (f == null) {
                    out.println("//// " + args[i] + ": no function");
                    continue;
                }
                out.println("//// " + f.getName() + " @ " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses());
                out.println("// callers: " + callersOf(f));
                out.println("// callees: " + calleesOf(f));
                DecompileResults r = decomp.decompileFunction(f, 120, monitor);
                if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
                else out.println("// decompile failed: " + (r != null ? r.getErrorMessage() : "null"));
                out.println();
                out.flush();
            }
        } finally {
            out.close();
            decomp.dispose();
        }
        println("DumpFuncs done");
    }
}
