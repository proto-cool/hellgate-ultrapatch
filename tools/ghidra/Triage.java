import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;
import java.util.*;

public class Triage extends GhidraScript {
    DecompInterface dec;

    String dc(Function f) {
        try {
            DecompileResults r = dec.decompileFunction(f, 60, monitor);
            if (r != null && r.decompileCompleted()) return r.getDecompiledFunction().getC();
            return "// decompile failed";
        } catch (Exception e) { return "// " + e; }
    }

    void show(long addr, String label) {
        Address a = toAddr(addr);
        Function f = getFunctionContaining(a);
        println("########## " + label + " @ " + a);
        if (f == null) { println("  NO FUNCTION"); return; }
        println("  func " + f.getName() + "  " + f.getEntryPoint() + " - " + f.getBody().getMaxAddress()
                + "  sig: " + f.getSignature().getPrototypeString());
        println(dc(f));
    }

    void callers(long addr, String label, int depth) {
        println("########## CALLERS of " + label + " @ " + toAddr(addr));
        Set<Function> seen = new HashSet<>();
        Deque<Object[]> q = new ArrayDeque<>();
        Function f0 = getFunctionContaining(toAddr(addr));
        if (f0 == null) { println("  no function"); return; }
        q.add(new Object[]{f0, 0});
        while (!q.isEmpty()) {
            Object[] it = q.poll();
            Function f = (Function) it[0]; int d = (Integer) it[1];
            if (!seen.add(f) || d > depth) continue;
            StringBuilder ind = new StringBuilder();
            for (int i = 0; i < d; i++) ind.append("    ");
            println(ind.toString() + f.getEntryPoint().toString() + "  " + f.getName());
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                Function c = getFunctionContaining(r.getFromAddress());
                if (c != null) q.add(new Object[]{c, d + 1});
                else println(ind.toString() + "    [ref from " + r.getFromAddress().toString() + " - no function: " + r.getReferenceType() + "]");
            }
        }
    }

    public void run() throws Exception {
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        String[] args = getScriptArgs();
        show(0x00870b10L, "hkMoppLongRayVirtualMachine::queryRayOnTree");
        show(0x00819ed0L, "hkMoppBvTreeShape::castRay (TtrcMopp #1)");
        show(0x00841800L, "hkWorld::castRay (TtRayCstCached)");
        show(0x007fb9e0L, "engine raycast wrapper");
        show(0x005d31f1L, "GAME caller of wrapper");
        callers(0x007fb9e0L, "engine raycast wrapper", 4);
        dec.dispose();
    }
}
