import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;
import java.util.*;

public class Callers extends GhidraScript {
    DecompInterface dec;
    String dc(Function f) {
        try {
            DecompileResults r = dec.decompileFunction(f, 60, monitor);
            if (r != null && r.decompileCompleted()) return r.getDecompiledFunction().getC();
        } catch (Exception e) {}
        return "// decompile failed";
    }
    public void run() throws Exception {
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        long target = Long.decode(getScriptArgs()[0]);
        boolean full = getScriptArgs().length > 1 && getScriptArgs()[1].equals("full");
        Function f0 = getFunctionContaining(toAddr(target));
        println("TARGET " + f0.getEntryPoint() + " " + f0.getName());
        List<Function> direct = new ArrayList<>();
        for (Reference r : getReferencesTo(f0.getEntryPoint())) {
            Function c = getFunctionContaining(r.getFromAddress());
            println("  ref " + r.getReferenceType() + " from " + r.getFromAddress()
                    + (c == null ? " [no func]" : "  in " + c.getEntryPoint() + " " + c.getName()));
            if (c != null && !direct.contains(c)) direct.add(c);
        }
        println("== " + direct.size() + " distinct direct callers ==");
        for (Function c : direct) {
            println("###### CALLER " + c.getEntryPoint() + " " + c.getName()
                    + "  size=" + c.getBody().getNumAddresses());
            // who calls the caller
            for (Reference r2 : getReferencesTo(c.getEntryPoint())) {
                Function cc = getFunctionContaining(r2.getFromAddress());
                println("    <- " + r2.getReferenceType() + " " + r2.getFromAddress()
                        + (cc == null ? " [no func]" : "  " + cc.getEntryPoint() + " " + cc.getName()));
            }
            if (full) println(dc(c));
        }
        dec.dispose();
    }
}
