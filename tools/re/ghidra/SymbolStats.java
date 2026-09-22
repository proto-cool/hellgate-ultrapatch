import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
public class SymbolStats extends GhidraScript {
    public void run() throws Exception {
        int user = 0, rtti = 0, vft = 0, imp = 0, classes = 0, total = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            total++;
            SourceType st = f.getSymbol().getSource();
            if (st == SourceType.USER_DEFINED) user++;
            else if (st == SourceType.IMPORTED) imp++;
            else if (st == SourceType.ANALYSIS) rtti++;
        }
        for (Symbol s : currentProgram.getSymbolTable().getAllSymbols(false))
            if (s.getName().contains("vftable")) vft++;
        for (ghidra.program.model.symbol.Namespace ns : new ghidra.program.model.symbol.Namespace[0]) classes++;
        java.util.Iterator<GhidraClass> ci = currentProgram.getSymbolTable().getClassNamespaces();
        while (ci.hasNext()) { ci.next(); classes++; }
        println("functions=" + total + " user-named=" + user + " analysis-named=" + rtti + " imported=" + imp + " vftables=" + vft + " classes=" + classes);
    }
}
