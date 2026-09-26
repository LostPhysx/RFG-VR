// Dumps the instruction listing of the given functions (address, bytes, mnemonic) into a file.
// Usage (headless): -postScript DumpListing.java <out.txt> <hexaddr> [<hexaddr> ...]
// @category rfg-vr
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import java.io.FileWriter;
import java.io.PrintWriter;

public class DumpListing extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DumpListing <out.txt> <hexaddr>...");
            return;
        }
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; i++) {
                long a = Long.parseLong(args[i].replaceFirst("^0[xX]", ""), 16);
                Function f = getFunctionContaining(toAddr(a));
                if (f == null) {
                    // Not a known function (e.g. a callback label): disassemble linearly until RET, max 1500.
                    out.println(";;;; " + args[i] + ": no function, linear listing");
                    Address cur = toAddr(a);
                    if (getInstructionAt(cur) == null) disassemble(cur);
                    for (int n = 0; n < 1500; n++) {
                        Instruction ins = getInstructionAt(cur);
                        if (ins == null) {
                            disassemble(cur);
                            ins = getInstructionAt(cur);
                            if (ins == null) break;
                        }
                        StringBuilder bytes = new StringBuilder();
                        for (byte b : ins.getBytes()) bytes.append(String.format("%02X", b & 0xff));
                        out.println(String.format("%s  %-24s %s", ins.getAddress(), bytes, ins));
                        if (ins.getMnemonicString().startsWith("RET")) break;
                        cur = ins.getAddress().add(ins.getLength());
                    }
                    out.println();
                    continue;
                }
                out.println(";;;; " + f.getName() + " @ " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses());
                InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
                while (it.hasNext() && !monitor.isCancelled()) {
                    Instruction ins = it.next();
                    StringBuilder bytes = new StringBuilder();
                    for (byte b : ins.getBytes()) bytes.append(String.format("%02X", b & 0xff));
                    out.println(String.format("%s  %-24s %s", ins.getAddress(), bytes, ins));
                }
                out.println();
            }
        }
        println("DumpListing done");
    }
}
