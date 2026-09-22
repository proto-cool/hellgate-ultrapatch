// Decompile functions by name (or 0x address) into <outdir>/<name>.c.
//   analyzeHeadless ... -postScript Decomp.java <outdir> <name|0xaddr>...
// Names are matched against every symbol in the program; a name with several
// matches writes one file per match. Unknown names are reported, not fatal.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;
import java.io.*;
import java.util.*;

public class Decomp extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) { println("usage: Decomp <outdir> <name|0xaddr>..."); return; }
        File dir = new File(args[0]);
        dir.mkdirs();
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        FunctionManager FM = currentProgram.getFunctionManager();
        SymbolTable ST = currentProgram.getSymbolTable();
        for (int i = 1; i < args.length; i++) {
            List<Function> fs = new ArrayList<>();
            if (args[i].startsWith("0x")) {
                Function f = FM.getFunctionContaining(toAddr(Long.decode(args[i])));
                if (f != null) fs.add(f);
            } else {
                for (Symbol s : ST.getSymbols(args[i])) {
                    Function f = FM.getFunctionAt(s.getAddress());
                    if (f != null) fs.add(f);
                }
            }
            if (fs.isEmpty()) { println("no function for " + args[i]); continue; }
            for (Function f : fs) {
                DecompileResults r = dec.decompileFunction(f, 120, monitor);
                String body = (r != null && r.decompileCompleted()) ? r.getDecompiledFunction().getC() : "// decompile failed\n";
                String fn = f.getName().replaceAll("[^A-Za-z0-9_]", "_") + (fs.size() > 1 ? "_" + f.getEntryPoint() : "") + ".c";
                try (PrintWriter w = new PrintWriter(new FileWriter(new File(dir, fn)))) {
                    w.println("// " + f.getName() + " @ " + f.getEntryPoint() + "  (Ghidra 12.1.3, hg_sp.exe = Hellgate_sp_x86.exe 2018-11-27)");
                    String c = f.getComment();
                    if (c != null) w.println("// " + c.replace("\n", "\n// "));
                    // callers, so the reader knows where it sits
                    Set<String> callers = new TreeSet<>();
                    for (Function cf : f.getCallingFunctions(monitor)) callers.add(cf.getName() + "@" + cf.getEntryPoint());
                    w.println("// callers (" + callers.size() + "): " + String.join(", ", callers));
                    w.println(body);
                }
                println("wrote " + fn + " (" + body.length() + " chars)");
            }
        }
        dec.dispose();
    }
}
