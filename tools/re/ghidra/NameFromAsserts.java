// Symbol recovery from the exe's own assert strings.
//
// The Flagship code base asserts with macros that stringify the expression
// (ASSERT_RETURN( dx9_EffectNew( &id, ... ) )) and pass __FILE__ / __LINE__.
// Compiled, that leaves three things next to each other in the caller:
//
//     call  <callee>              ; the asserted expression
//     test  eax, eax / cmp ...
//     jxx   ok
//     push  <line>                ; __LINE__
//     push  offset ".\Source\DxC\dxC_effect.cpp"
//     push  offset "dx9_EffectNew( &id, ... )"
//     call  <assert report>
//
// So for each expression string we walk back from the instruction that
// references it to the nearest preceding direct CALL inside the same function
// and vote that its target is called <first identifier of the expression>.
// Targets are renamed when the votes agree (majority; ties are skipped) and
// the function still carries a default FUN_ name. Every function that
// references a __FILE__ string is tagged with that file, and the __LINE__
// push nearest each reference gives an approximate line.
//
// Strings of the form "name()" referenced from a function are recorded as a
// self-name candidate only; they are used in messages and could be either the
// caller or the callee, so they never rename anything here.
//
// Output: a CSV given as the first script argument, one row per fact:
//   kind,address,name,file,line,votes,detail
// kinds: callee (renamed), callee-kept (name agreed but function already
// named), conflict, self, file.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import ghidra.program.model.scalar.Scalar;
import java.io.*;
import java.util.*;
import java.util.regex.*;

public class NameFromAsserts extends GhidraScript {
    Listing L; FunctionManager FM; ReferenceManager RM;

    Instruction prevCall(Instruction from, Function f, int limit) {
        Instruction ins = from.getPrevious();
        for (int i = 0; ins != null && i < limit; i++, ins = ins.getPrevious()) {
            if (!f.getBody().contains(ins.getAddress())) return null;
            if (ins.getFlowType().isCall()) return ins;
        }
        return null;
    }

    Function callTarget(Instruction call) {
        Address[] flows = call.getFlows();
        if (flows == null || flows.length != 1) return null;
        Function t = FM.getFunctionAt(flows[0]);
        if (t == null) return null;
        while (t.isThunk() && t.getThunkedFunction(false) != null) t = t.getThunkedFunction(false);
        if (t.isExternal()) return null;
        return t;
    }

    /*
     * __LINE__: the arguments go on the stack right to left, so the line is
     * the PUSH immediately before the __FILE__ push, which is immediately
     * before the expression push. Anything further away is some other
     * argument, which is how the first version of this reported "line 1".
     */
    long nearbyLine(Instruction ref, int back) {
        Instruction ins = ref;
        for (int i = 0; ins != null && i < back; i++) ins = ins.getPrevious();
        /* the compiler sometimes interleaves a register load; allow one slip */
        for (int i = 0; ins != null && i < 2; i++, ins = ins.getPrevious()) {
            Long v = pushImm(ins);
            if (v != null) return v >= 10 ? v : -1;
        }
        return -1;
    }

    Long pushImm(Instruction ins) {
        if (!ins.getMnemonicString().equals("PUSH")) return null;
        Object[] ops = ins.getOpObjects(0);
        if (ops.length == 1 && ops[0] instanceof Scalar) {
            long v = ((Scalar) ops[0]).getUnsignedValue();
            if (v >= 1 && v <= 40000) return v;
        }
        return null;
    }

    public void run() throws Exception {
        L = currentProgram.getListing(); FM = currentProgram.getFunctionManager();
        RM = currentProgram.getReferenceManager();
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(args.length > 0 ? args[0] : "/tmp/names.csv"));
        out.println("kind,address,name,file,line,votes,detail");

        Pattern callExpr = Pattern.compile("^([A-Za-z_][A-Za-z0-9_]*)\\s*\\((.*)\\)\\s*$", Pattern.DOTALL);
        Pattern fileStr  = Pattern.compile("^(\\.\\\\)?[A-Za-z0-9_\\\\]+\\.(cpp|h|inl)$");
        Pattern selfStr  = Pattern.compile("^([A-Za-z_][A-Za-z0-9_]*)\\(\\)$");

        // target function -> name -> votes
        Map<Long, Map<String, Integer>> votes = new HashMap<>();
        Map<Long, String> voteDetail = new HashMap<>();
        // function -> file, line (smallest line seen)
        Map<Long, String> fileOf = new HashMap<>();
        Map<Long, Long> lineOf = new HashMap<>();
        Map<Long, Set<String>> selfOf = new HashMap<>();

        int nexpr = 0, nfile = 0;
        for (Data d : L.getDefinedData(true)) {
            DataType dt = d.getDataType();
            if (!(dt instanceof StringDataType) && !(dt instanceof TerminatedStringDataType)) continue;
            Object v = d.getValue();
            if (!(v instanceof String)) continue;
            String s = ((String) v).trim();
            boolean isFile = fileStr.matcher(s).matches();
            Matcher self = selfStr.matcher(s);
            Matcher ce = callExpr.matcher(s);
            boolean isSelf = self.matches();
            boolean isCall = !isSelf && ce.matches() && s.length() < 600;
            if (!isFile && !isSelf && !isCall) continue;
            String callee = isCall ? ce.group(1) : null;
            // skip expressions whose head is a keyword or a type, not a function
            if (isCall && (callee.equals("if") || callee.equals("sizeof") || callee.equals("return")
                    || callee.equals("while") || callee.equals("for") || callee.equals("switch")
                    || callee.equals("V") || callee.equals("SUCCEEDED") || callee.equals("FAILED"))) continue;

            ReferenceIterator it = RM.getReferencesTo(d.getAddress());
            while (it.hasNext()) {
                Reference r = it.next();
                Instruction ref = L.getInstructionAt(r.getFromAddress());
                if (ref == null) continue;
                Function f = FM.getFunctionContaining(ref.getAddress());
                if (f == null) continue;
                long fe = f.getEntryPoint().getOffset();
                if (isFile) {
                    nfile++;
                    fileOf.merge(fe, s, (a, b) -> a.equals(b) ? a : a + "|" + b);
                    long line = nearbyLine(ref, 1);
                    if (line > 0) lineOf.merge(fe, line, Math::min);
                    continue;
                }
                if (isSelf) {
                    selfOf.computeIfAbsent(fe, k -> new TreeSet<>()).add(self.group(1));
                    continue;
                }
                nexpr++;
                Instruction call = prevCall(ref, f, 48);
                if (call == null) continue;
                Function t = callTarget(call);
                if (t == null) continue;
                long te = t.getEntryPoint().getOffset();
                votes.computeIfAbsent(te, k -> new HashMap<>()).merge(callee, 1, Integer::sum);
                voteDetail.putIfAbsent(te, "from " + f.getEntryPoint() + ": " + s.replace(',', ';').replace('\n', ' '));
                long line = nearbyLine(ref, 2);
                if (line > 0) lineOf.merge(fe, line, Math::min);
            }
        }

        int renamed = 0, kept = 0, conflicts = 0;
        for (Map.Entry<Long, Map<String, Integer>> e : votes.entrySet()) {
            Function t = FM.getFunctionAt(toAddr(e.getKey()));
            if (t == null) continue;
            String best = null; int bestN = 0, total = 0; boolean tie = false;
            for (Map.Entry<String, Integer> ve : e.getValue().entrySet()) {
                total += ve.getValue();
                if (ve.getValue() > bestN) { best = ve.getKey(); bestN = ve.getValue(); tie = false; }
                else if (ve.getValue() == bestN) tie = true;
            }
            String detail = voteDetail.get(e.getKey());
            if (tie || best == null || bestN * 2 <= total) {
                conflicts++;
                out.println("conflict," + t.getEntryPoint() + "," + t.getName() + ",,," + total + ","
                        + e.getValue().toString().replace(',', ';'));
                continue;
            }
            if (t.getSymbol().getSource() == SourceType.DEFAULT) {
                t.setName(best, SourceType.USER_DEFINED);
                t.setComment("named from " + bestN + "/" + total + " assert expression(s), e.g. " + detail);
                renamed++;
                out.println("callee," + t.getEntryPoint() + "," + best + ",,," + bestN + "/" + total + ",\"" + detail + "\"");
            } else {
                kept++;
                out.println("callee-kept," + t.getEntryPoint() + "," + t.getName()
                        + (t.getName().equals(best) ? "" : " (" + best + ")") + ",,," + bestN + "/" + total + ",\"" + detail + "\"");
            }
        }
        for (Map.Entry<Long, String> e : fileOf.entrySet()) {
            Function f = FM.getFunctionAt(toAddr(e.getKey()));
            if (f == null) continue;
            Long line = lineOf.get(e.getKey());
            String plate = f.getComment();
            String tag = "FILE " + e.getValue() + (line != null ? " line ~" + line : "");
            if (plate == null || !plate.contains("FILE ")) f.setComment(plate == null ? tag : plate + "\n" + tag);
            out.println("file," + f.getEntryPoint() + "," + f.getName() + "," + e.getValue() + "," + (line != null ? line : "") + ",,");
        }
        for (Map.Entry<Long, Set<String>> e : selfOf.entrySet()) {
            Function f = FM.getFunctionAt(toAddr(e.getKey()));
            if (f == null) continue;
            out.println("self," + f.getEntryPoint() + "," + f.getName() + ",,,," + String.join("|", e.getValue()));
        }
        out.close();
        println("expressions=" + nexpr + " file refs=" + nfile + " targets voted=" + votes.size()
                + " renamed=" + renamed + " kept=" + kept + " conflicts=" + conflicts
                + " files tagged=" + fileOf.size() + " self candidates=" + selfOf.size());
    }
}
