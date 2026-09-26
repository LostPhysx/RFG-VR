// Lists instructions whose memory operand uses one of the given displacements ([reg + disp]),
// with the containing function. Finds users of a struct field.
// Usage (headless): -postScript FindDisplacement.java <out.txt> <hexdisp> [<hexdisp> ...]
// @category rfg-vr
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.HashSet;
import java.util.Set;

public class FindDisplacement extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        Set<Long> disps = new HashSet<>();
        for (int i = 1; i < args.length; i++) disps.add(Long.parseLong(args[i].replaceFirst("^0[xX]", ""), 16));
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            InstructionIterator it = currentProgram.getListing().getInstructions(true);
            while (it.hasNext() && !monitor.isCancelled()) {
                Instruction ins = it.next();
                for (int op = 0; op < ins.getNumOperands(); op++) {
                    String rep = ins.getDefaultOperandRepresentation(op);
                    if (!rep.contains("[") || !rep.contains("+")) continue;
                    for (Object o : ins.getOpObjects(op)) {
                        if (o instanceof Scalar && disps.contains(((Scalar) o).getUnsignedValue())) {
                            Function f = getFunctionContaining(ins.getAddress());
                            out.println(ins.getAddress() + "  " + ins + "   in " + (f == null ? "?" : f.getName()));
                        }
                    }
                }
            }
        }
    }
}
