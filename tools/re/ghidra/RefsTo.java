// List code references to the given addresses (globals), with the containing function.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
public class RefsTo extends GhidraScript {
    public void run() throws Exception {
        for (String a : getScriptArgs()) {
            Address ad = toAddr(Long.decode(a));
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(ad);
            println("== refs to " + ad);
            while (it.hasNext()) {
                Reference r = it.next();
                Instruction ins = getInstructionAt(r.getFromAddress());
                Function f = getFunctionContaining(r.getFromAddress());
                println("  " + r.getFromAddress() + "  " + (ins == null ? "(data)" : ins.toString()) + "   in " + (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()) + "  " + r.getReferenceType());
            }
        }
    }
}
