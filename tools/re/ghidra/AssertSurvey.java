// Survey: how much of the exe could be named from its assert strings.
// Counts functions, assert-expression strings ("name( ... )"), the functions
// that reference them, __FILE__ strings, and the distinct callee names.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import java.util.*;
import java.util.regex.*;

public class AssertSurvey extends GhidraScript {
    public void run() throws Exception {
        Listing L = currentProgram.getListing();
        FunctionManager FM = currentProgram.getFunctionManager();
        ReferenceManager RM = currentProgram.getReferenceManager();
        Pattern call = Pattern.compile("^([A-Za-z_][A-Za-z0-9_:]*)\\s*\\(.*");
        Pattern file = Pattern.compile("^(\\.\\\\)?[A-Za-z0-9_\\\\]+\\.(cpp|h)$");
        int nfunc = 0;
        for (Function f : FM.getFunctions(true)) nfunc++;
        int nstr = 0, ncallstr = 0, nfilestr = 0, nref = 0;
        Set<Long> funcsWithAssert = new HashSet<>();
        Set<Long> funcsWithFile = new HashSet<>();
        Map<String, Integer> callee = new TreeMap<>();
        Set<String> files = new TreeSet<>();
        for (Data d : L.getDefinedData(true)) {
            if (!(d.getDataType() instanceof StringDataType) && !(d.getDataType() instanceof TerminatedStringDataType)) continue;
            Object v = d.getValue();
            if (!(v instanceof String)) continue;
            String s = (String) v;
            nstr++;
            Matcher m = call.matcher(s);
            boolean isCall = m.matches() && s.endsWith(")") && s.length() < 400;
            boolean isFile = file.matcher(s).matches();
            if (!isCall && !isFile) continue;
            ReferenceIterator it = RM.getReferencesTo(d.getAddress());
            boolean any = false;
            while (it.hasNext()) {
                Reference r = it.next();
                Function f = FM.getFunctionContaining(r.getFromAddress());
                if (f == null) continue;
                any = true; nref++;
                if (isCall) funcsWithAssert.add(f.getEntryPoint().getOffset());
                if (isFile) funcsWithFile.add(f.getEntryPoint().getOffset());
            }
            if (isCall) { ncallstr++; if (any) callee.merge(m.group(1), 1, Integer::sum); }
            if (isFile) { nfilestr++; files.add(s); }
        }
        println("functions=" + nfunc);
        println("strings=" + nstr + " call-expr strings=" + ncallstr + " file strings=" + nfilestr + " refs=" + nref);
        println("functions containing an assert expression=" + funcsWithAssert.size());
        println("functions referencing a __FILE__ string=" + funcsWithFile.size());
        println("distinct callee names in expressions=" + callee.size());
        println("distinct source files=" + files.size());
        int shown = 0;
        for (String fn : files) { if (shown++ < 200) println("  file " + fn); }
        List<Map.Entry<String,Integer>> top = new ArrayList<>(callee.entrySet());
        top.sort((a, b) -> b.getValue() - a.getValue());
        for (int i = 0; i < Math.min(60, top.size()); i++) println("  callee " + top.get(i).getKey() + " x" + top.get(i).getValue());
    }
}
