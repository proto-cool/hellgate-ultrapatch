import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;

public class Show extends GhidraScript {
    public void run() throws Exception {
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        for (String a : getScriptArgs()) {
            Address ad = toAddr(Long.decode(a));
            Function f = getFunctionContaining(ad);
            println("########## " + a + " -> " + (f == null ? "NO FUNC" : f.getEntryPoint() + " " + f.getName()));
            if (f == null) continue;
            DecompileResults r = dec.decompileFunction(f, 90, monitor);
            println((r != null && r.decompileCompleted()) ? r.getDecompiledFunction().getC() : "// fail");
        }
        dec.dispose();
    }
}
