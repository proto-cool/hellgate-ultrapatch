// List instructions that use a given displacement (e.g. 0x12cc) as a memory operand offset.
//   Decomp-style usage: -postScript FindOffsetRefs.java 0x12cc 0x12cd
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import java.util.*;
public class FindOffsetRefs extends GhidraScript {
    public void run() throws Exception {
        Set<Long> want = new HashSet<>();
        for (String a : getScriptArgs()) want.add(Long.decode(a));
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            for (int i = 0; i < ins.getNumOperands(); i++) {
                for (Object o : ins.getOpObjects(i)) {
                    if (o instanceof Scalar && want.contains(((Scalar) o).getUnsignedValue())) {
                        Function f = getFunctionContaining(ins.getAddress());
                        println(ins.getAddress() + "  " + ins + "   in " + (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
                    }
                }
            }
        }
    }
}
