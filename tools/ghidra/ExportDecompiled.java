// Post-analysis: decompile every function into OUT_DIR/<address>.c, and list each one with its size
// and outcome in OUT_DIR/index.tsv. A function the decompiler fails on gets a file holding the error,
// so the index and the directory always list the same functions.
//
// The "Removing unreachable block" warnings are dropped. With the FPU mode pinned, they name the
// other-width path of each fmov and say nothing about the function.
// Next to each <address>.c it writes <address>.json: the prototype, every local and parameter with
// its storage and type, and every global the function uses with the type the decompiler gave it
// there. tools/decomp/convert.py needs these to turn the C into code that compiles.
//
// The C is printed from the decompiler's tokens, with one change: Ghidra prints a value moved
// between a float and an integer register unchanged as the same cast as a conversion, (uint)fVar1.
// So it prints the low word of a register pair, (float)xd12. Such a cast, a CAST, SUBPIECE or
// extension between a float and a non-float type, is printed as pun<uint>(fVar1). So is an
// argument whose type is a float where the callee takes a word, or the reverse, and a piece of a
// variable that holds a float is marked x._4_4f_.
// Usage (headless): -postScript ExportDecompiled.java <out_dir> [timeout_seconds]
//@category Dreamcast
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.regex.Pattern;

import ghidra.app.decompiler.ClangBreak;
import ghidra.app.decompiler.ClangCommentToken;
import ghidra.app.decompiler.ClangFuncNameToken;
import ghidra.app.decompiler.ClangNode;
import ghidra.app.decompiler.ClangOpToken;
import ghidra.app.decompiler.ClangStatement;
import ghidra.app.decompiler.ClangSyntaxToken;
import ghidra.app.decompiler.ClangToken;
import ghidra.app.decompiler.ClangTokenGroup;
import ghidra.app.decompiler.ClangTypeToken;
import ghidra.app.decompiler.ClangVariableToken;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.parallel.DecompileConfigurer;
import ghidra.app.decompiler.parallel.DecompilerCallback;
import ghidra.app.decompiler.parallel.ParallelDecompiler;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.AbstractFloatDataType;
import ghidra.program.model.data.AbstractIntegerDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.Pointer;
import ghidra.program.model.data.DefaultDataType;
import ghidra.program.model.data.Undefined;
import ghidra.program.model.data.TypeDef;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.Program;
import ghidra.program.model.listing.VariableStorage;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.HighSymbol;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.Varnode;
import ghidra.util.task.TaskMonitor;

public class ExportDecompiled extends GhidraScript {

    private static final Pattern UNREACHABLE =
        Pattern.compile("(?m)^/\\* WARNING: Removing unreachable block \\(ram,0x[0-9a-f]+\\) \\*/\\n");

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args.length > 0 ? args[0] : "decomp");
        int timeout = args.length > 1 ? Integer.parseInt(args[1]) : 120;
        out.mkdirs();

        DecompileConfigurer cfg = new DecompileConfigurer() {
            @Override
            public void configure(DecompInterface d) {
                DecompileOptions o = new DecompileOptions();
                o.grabFromProgram(currentProgram);
                d.setOptions(o);
                d.toggleCCode(true);
                d.toggleSyntaxTree(true);
                d.setSimplificationStyle("decompile");
            }
        };
        DecompilerCallback<String[]> cb = new DecompilerCallback<String[]>(currentProgram, cfg) {
            @Override
            public String[] process(DecompileResults r, TaskMonitor m) throws Exception {
                Function f = r.getFunction();
                String addr = f.getEntryPoint().toString();
                String size = Long.toString(f.getBody().getNumAddresses());
                String header = "// " + f.getName() + " @ 0x" + addr + ", " + size + " bytes\n";
                if (r.decompileCompleted() && r.getDecompiledFunction() != null) {
                    String c = UNREACHABLE.matcher(render(r.getCCodeMarkup(), f.getProgram())).replaceAll("");
                    write(new File(out, addr + ".c"), header + c.replaceFirst("^\\n+", "\n"));
                    write(new File(out, addr + ".json"),
                        sidecar(f, r.getHighFunction(), r.getDecompiledFunction().getSignature()));
                    return new String[] { addr, f.getName(), size, "ok" };
                }
                String err = String.valueOf(r.getErrorMessage()).trim().replace('\n', ' ');
                write(new File(out, addr + ".c"), header + "// decompile failed: " + err + "\n");
                // The prototype alone, so the port can declare the function and fail where it is called.
                write(new File(out, addr + ".json"), "{\"name\": " + q(f.getName()) + ", \"entry\": "
                    + q(addr) + ", \"signature\": " + q(f.getSignature().getPrototypeString() + ";")
                    + ", \"failed\": true, \"locals\": [], \"globals\": [], \"calls\": []}\n");
                return new String[] { addr, f.getName(), size, "failed: " + err };
            }
        };
        cb.setTimeout(timeout);

        // EXPORT_ONLY=8c0a9664,...: only these functions, for a check of a change to the printing.
        String only = System.getenv("EXPORT_ONLY");
        List<Function> fns = new ArrayList<>();
        currentProgram.getFunctionManager().getFunctions(true).forEach(f -> {
            if (only == null || Set.of(only.split(",")).contains(f.getEntryPoint().toString())) {
                fns.add(f);
            }
        });
        List<String[]> rows;
        try {
            rows = ParallelDecompiler.decompileFunctions(cb, fns, monitor);
        }
        finally {
            cb.dispose();
        }
        rows.sort((a, b) -> a[0].compareTo(b[0]));

        int ok = 0;
        try (PrintWriter w = new PrintWriter(new File(out, "index.tsv"))) {
            w.println("address\tname\tsize\tstatus");
            for (String[] row : rows) {
                w.println(String.join("\t", row));
                if (row[3].equals("ok")) {
                    ok++;
                }
            }
        }
        println("ExportDecompiled: " + ok + " of " + rows.size() + " functions decompiled -> " + out);
    }

    // ---- Printing the C from the tokens --------------------------------------------------------

    private static final Set<Integer> CONVERSIONS = Set.of(PcodeOp.CAST, PcodeOp.FLOAT_INT2FLOAT,
        PcodeOp.FLOAT_FLOAT2FLOAT, PcodeOp.FLOAT_TRUNC, PcodeOp.INT_ZEXT, PcodeOp.INT_SEXT,
        PcodeOp.SUBPIECE);
    // The ops that keep the bits: printed as casts, they convert nothing.
    private static final Set<Integer> BIT_MOVES = Set.of(PcodeOp.CAST, PcodeOp.SUBPIECE,
        PcodeOp.INT_ZEXT, PcodeOp.INT_SEXT);
    private static final Set<String> PREFIX = Set.of("*", "-", "!", "~", "&", "++", "--");

    private static String render(ClangTokenGroup markup, Program program) {
        List<ClangNode> nodes = new ArrayList<>();
        markup.flatten(nodes);
        List<ClangToken> t = new ArrayList<>();
        for (ClangNode n : nodes) {
            t.add((ClangToken) n);
        }
        String[] text = new String[t.size()];
        for (int i = 0; i < t.size(); i++) {
            ClangToken x = t.get(i);
            text[i] = x instanceof ClangBreak br ? "\n" + " ".repeat(br.getIndent())
                : x instanceof ClangCommentToken || x instanceof ClangSyntaxToken ? x.getText()
                : identifier(x.getText());
        }
        // A comparison with zero says which compare the machine code makes. fcmp/eq (FLOAT_EQUAL)
        // follows FPSCR.DN, where a denormal is 0: dn_zero. An integer test (INT_EQUAL) of a word
        // that Ghidra typed as a float prints the same 0.0, and compares all 32 bits: int_zero.
        for (int i = 0; i < t.size(); i++) {
            if (!(t.get(i) instanceof ClangVariableToken v) || v.getVarnode() == null || !v.getVarnode().isConstant()
                    || v.getVarnode().getOffset() != 0 || v.getPcodeOp() == null) {
                continue;
            }
            int code = v.getPcodeOp().getOpcode();
            if (code == PcodeOp.FLOAT_EQUAL || code == PcodeOp.FLOAT_NOTEQUAL) {
                text[i] = "dn_zero";
            }
            else if ((code == PcodeOp.INT_EQUAL || code == PcodeOp.INT_NOTEQUAL) && t.get(i).getText().contains(".")) {
                text[i] = "int_zero";
            }
        }
        // An equality test of a signed byte or halfword with a constant from 0x80 or 0x8000 up: the
        // machine code compares the low bits, and C promotes the operand with its sign, so
        // param_1[0x1c] != 0xff was always true for a char. The constant gets the operand's type.
        for (int i = 0; i < t.size(); i++) {
            if (!(t.get(i) instanceof ClangVariableToken v) || v.getVarnode() == null || !v.getVarnode().isConstant()
                    || v.getPcodeOp() == null) {
                continue;
            }
            PcodeOp op = v.getPcodeOp();
            int code = op.getOpcode();
            if (code != PcodeOp.INT_EQUAL && code != PcodeOp.INT_NOTEQUAL) {
                continue;
            }
            Varnode other = op.getInput(0) == v.getVarnode() ? op.getInput(1) : op.getInput(0);
            int size = other.getSize();
            long value = v.getVarnode().getOffset();
            if ((size != 1 && size != 2) || value < (1L << (8 * size - 1)) || other.getHigh() == null) {
                continue;
            }
            DataType dt = other.getHigh().getDataType();
            while (dt instanceof TypeDef td) {
                dt = td.getBaseDataType();
            }
            if (dt instanceof AbstractIntegerDataType it && it.isSigned() && !text[i].startsWith("-")) {
                text[i] = "(" + (size == 1 ? "char" : "short") + ")" + text[i];
            }
        }
        // A piece of a variable that holds a float, x._4_4_, is marked x._4_4f_: C would read the
        // piece as an integer.
        for (int i = 0; i < t.size(); i++) {
            if (!(t.get(i) instanceof ClangSyntaxToken) || !t.get(i).getText().matches("_\\d+_\\d+_")) {
                continue;
            }
            for (int k = i - 1; k >= 0 && k >= i - 4; k--) {
                if (t.get(k) instanceof ClangVariableToken v) {
                    Varnode vn = v.getVarnode();
                    if (vn != null && vn.getHigh() != null && isFloatType(vn.getHigh().getDataType())) {
                        text[i] = t.get(i).getText().replaceAll("_$", "f_");
                    }
                    break;
                }
            }
        }
        // An argument passed where the callee takes a float, or a float passed for a word, moves its
        // bits: a table of floats typed as pointers is loaded with fmov.s into fr4.
        for (int i = 0; i < t.size(); i++) {
            // A direct call, f(...), or a call through a pointer, (*p)(...).
            int open;
            if (t.get(i) instanceof ClangFuncNameToken fn && fn.getPcodeOp() != null
                    && fn.getPcodeOp().getOpcode() == PcodeOp.CALL) {
                open = skipBlank(t, i + 1);
            }
            else if (t.get(i).getText().equals(")") && skipBlank(t, i + 1) < t.size()
                    && t.get(skipBlank(t, i + 1)).getText().equals("(")) {
                open = skipBlank(t, i + 1);
            }
            else {
                continue;
            }
            if (open >= t.size() || !t.get(open).getText().equals("(")) {
                continue;
            }
            int close = matching(t, open);
            if (close < 0) {
                continue;
            }
            PcodeOp call = callOf(t, open, close);
            Function callee = call == null ? null : callee(call, program);
            if (callee == null) {
                continue;
            }
            int arg = 0, start = open + 1, depth = 0;
            for (int k = open + 1; k <= close && close > 0; k++) {
                String x = t.get(k).getText();
                if (x.equals("(") || x.equals("[")) {
                    depth++;
                }
                else if ((x.equals(")") || x.equals("]")) && k != close) {
                    depth--;
                }
                if ((x.equals(",") && depth == 0) || k == close) {
                    int a0 = skipBlank(t, start);
                    // Ghidra 12.2 counts parameters that getParameter does not return.
                    Parameter param = arg < callee.getParameterCount() ? callee.getParameter(arg) : null;
                    if (a0 < k && arg + 1 < call.getNumInputs() && param != null) {
                        boolean wantFloat = isFloatType(param.getDataType());
                        Varnode in = call.getInput(arg + 1);
                        if (!in.isConstant() && wantFloat != isFloat(in)) {
                            String target = wantFloat ? "float" : "uint";
                            text[a0] = "pun<" + target + ">(" + text[a0];
                            text[k - 1] = text[k - 1] + ")";
                        }
                    }
                    arg++;
                    start = k + 1;
                }
            }
        }
        // Innermost casts first, so that an outer cast knows the ops of the casts inside it.
        Map<Integer, PcodeOp> owned = new HashMap<>();
        for (int i = t.size() - 1; i >= 0; i--) {
            int close = castClose(t, i);
            if (close < 0) {
                continue;
            }
            int s = skipBlank(t, close + 1);
            int e = unaryEnd(t, s);
            if (e < 0) {
                continue;
            }
            PcodeOp op = castOp(t, s, e, owned, castsToPointer(t, i, close));
            if (op != null) {
                owned.put(i, op);
                // An int-to-float conversion of an undefined word: in the port such a word reads as a
                // float where one is expected, so the conversion says that it reads an integer.
                if (op.getOpcode() == PcodeOp.FLOAT_INT2FLOAT && isUndefined(op.getInput(0))) {
                    text[close] = text[close] + "(int)";
                }
            }
            // A float conversion is one, whatever the operand looks like: (int)_DAT_8c1d1540 truncates
            // a float, and as a piece of an overlapping global it had been printed as its bits. The
            // conversion must make what the cast names: in (int)(float)DAT_8c1d1524 the TRUNC is the
            // (int), and the (float) reads the bits of a global typed Angle.
            final boolean converts = op != null && isFloatConversion(op)
                && castsToFloat(t, i, close) == (op.getOpcode() != PcodeOp.FLOAT_TRUNC);
            if (!converts && ((op != null && isPun(op)) || isPartial(t, s, e) || isFloatConstant(t, i, close, s, e)
                    || isFloatOfBits(t, i, close, s, e) || (op == null && isBitsOfFloat(t, i, close, s, e)))) {
                // The argument pass above may have wrapped the cast already: keep what it added.
                text[i] = text[i].substring(0, text[i].length() - 1) + "pun<";
                text[close] = text[close].replaceFirst("^\\)", ">(");
                text[e - 1] = text[e - 1] + ")";
            }
        }
        StringBuilder b = new StringBuilder();
        for (String x : text) {
            b.append(x);
        }
        return b.append("\n").toString();
    }

    // Ghidra's own C text spells a name's other characters as underscores: a type's ".conflict"
    // suffix, or s_Now,Loading..._8c0d3a8e for a string's label.
    private static String identifier(String s) {
        if (s.isEmpty() || !(Character.isLetter(s.charAt(0)) || s.charAt(0) == '_')) {
            return s;
        }
        return s.replaceAll("[^A-Za-z0-9_ *]", "_");
    }

    // The call whose arguments are the tokens between open and close: an op that a token there
    // carries, or one its output goes to.
    private static PcodeOp callOf(List<ClangToken> t, int open, int close) {
        for (int k = open + 1; k < close; k++) {
            PcodeOp op = t.get(k).getPcodeOp();
            if (op == null) {
                continue;
            }
            if (op.getOpcode() == PcodeOp.CALL || op.getOpcode() == PcodeOp.CALLIND) {
                return op;
            }
            Varnode out = op.getOutput();
            if (out != null) {
                Iterator<PcodeOp> it = out.getDescendants();
                while (it.hasNext()) {
                    PcodeOp d = it.next();
                    if (d.getOpcode() == PcodeOp.CALL || d.getOpcode() == PcodeOp.CALLIND) {
                        return d;
                    }
                }
            }
        }
        return null;
    }

    // A call's target: the function called directly, or the one whose address a constant word
    // holds, for a call through a literal-pool slot.
    private static Function callee(PcodeOp call, Program program) {
        if (call.getOpcode() == PcodeOp.CALL) {
            return program.getFunctionManager().getFunctionAt(call.getInput(0).getAddress());
        }
        Varnode target = call.getInput(0);
        // Through copies and casts to the slot.
        for (int k = 0; k < 4 && target.getDef() != null
                && (target.getDef().getOpcode() == PcodeOp.COPY || target.getDef().getOpcode() == PcodeOp.CAST);
                k++) {
            target = target.getDef().getInput(0);
        }
        PcodeOp load = target.getDef();
        long slotOffset;
        if (target.getAddress().isMemoryAddress()) {
            slotOffset = target.getOffset();  // the slot itself, read as a global
        }
        else if (load != null && load.getOpcode() == PcodeOp.LOAD && load.getInput(1).isConstant()) {
            slotOffset = load.getInput(1).getOffset();
        }
        else {
            return null;
        }
        try {
            ghidra.program.model.address.Address slot = program.getAddressFactory().getDefaultAddressSpace()
                .getAddress(slotOffset);
            long word = program.getMemory().getInt(slot) & 0xFFFFFFFFL;
            return program.getFunctionManager().getFunctionAt(
                program.getAddressFactory().getDefaultAddressSpace().getAddress(word));
        }
        catch (Exception e) {
            return null;
        }
    }

    private static boolean blank(ClangToken x) {
        return x instanceof ClangBreak || x.getText().isBlank();
    }

    private static int skipBlank(List<ClangToken> t, int k) {
        while (k < t.size() && blank(t.get(k))) {
            k++;
        }
        return k;
    }

    // If token i opens a cast, "(" type ")", the index of its ")"; otherwise -1.
    private static int castClose(List<ClangToken> t, int i) {
        if (!t.get(i).getText().equals("(")) {
            return -1;
        }
        int j = skipBlank(t, i + 1);
        // A cast is in a statement or in a condition, which is a plain group; a declaration's
        // parentheses, such as a prototype's (void), are in groups of their own kinds.
        if (j >= t.size() || !(t.get(j) instanceof ClangTypeToken)
                || !(t.get(j).Parent() instanceof ClangStatement
                    || t.get(j).Parent().getClass() == ClangTokenGroup.class)) {
            return -1;
        }
        for (int k = j + 1; k < t.size(); k++) {
            String x = t.get(k).getText();
            if (x.equals(")")) {
                return k;
            }
            if (!blank(t.get(k)) && !x.equals("*")) {
                return -1;
            }
        }
        return -1;
    }

    private static int matching(List<ClangToken> t, int k) {
        String open = t.get(k).getText(), close = open.equals("(") ? ")" : "]";
        int depth = 0;
        for (int i = k; i < t.size(); i++) {
            String x = t.get(i).getText();
            if (x.equals(open)) {
                depth++;
            }
            else if (x.equals(close) && --depth == 0) {
                return i;
            }
        }
        return -1;
    }

    // The end (exclusive) of the unary expression, a cast's operand, that starts at token s.
    private static int unaryEnd(List<ClangToken> t, int s) {
        int k = skipBlank(t, s);
        if (k >= t.size()) {
            return -1;
        }
        ClangToken x = t.get(k);
        if (x.getText().equals("(")) {
            int close = castClose(t, k);
            if (close >= 0) {
                return unaryEnd(t, close + 1);
            }
            int m = matching(t, k);
            return m < 0 ? -1 : postfix(t, m + 1);
        }
        if (x instanceof ClangOpToken && PREFIX.contains(x.getText())) {
            return unaryEnd(t, k + 1);
        }
        if (x.getText().equals(")") || x.getText().equals(";") || x.getText().equals(",")) {
            return -1;
        }
        return postfix(t, k + 1);
    }

    private static int postfix(List<ClangToken> t, int k) {
        while (true) {
            int j = skipBlank(t, k);
            if (j >= t.size()) {
                return k;
            }
            String x = t.get(j).getText();
            if (x.equals("->") || x.equals(".")) {
                k = skipBlank(t, j + 1) + 1;
            }
            else if (x.equals("[") || x.equals("(")) {
                int m = matching(t, j);
                if (m < 0) {
                    return k;
                }
                k = m + 1;
            }
            else {
                return k;
            }
        }
    }

    // The op a cast prints: the conversion that reads the value of its operand, tokens s to e.
    // The operand's tokens carry the ops that make its value; the casts inside it own theirs.
    // A cast to a value is not a cast to a pointer: the operand's pointer can feed another
    // expression's pointer cast, as param_1 + 0x10 feeds *(ushort *)(param_1 + 0x10) and
    // (uint)param_1[0x10] in FUN_8c0ae1a8.
    private static boolean isPointerCast(PcodeOp op) {
        return op.getOpcode() == PcodeOp.CAST && op.getOutput() != null && op.getOutput().getHigh() != null
            && op.getOutput().getHigh().getDataType() instanceof Pointer;
    }

    // (T *), a cast to a pointer.
    private static boolean castsToPointer(List<ClangToken> t, int open, int close) {
        for (int j = open + 1; j < close; j++) {
            if (t.get(j).getText().equals("*")) {
                return true;
            }
        }
        return false;
    }

    private static PcodeOp castOp(List<ClangToken> t, int s, int e, Map<Integer, PcodeOp> owned, boolean toPointer) {
        Set<PcodeOp> inside = new HashSet<>();
        for (int k = s; k < e; k++) {
            if (owned.containsKey(k)) {
                inside.add(owned.get(k));
            }
        }
        List<PcodeOp> frontier = new ArrayList<>();
        Set<PcodeOp> mine = new HashSet<>(inside);
        for (int k = s; k < e; k++) {
            PcodeOp op = t.get(k).getPcodeOp();
            if (op != null && !inside.contains(op)) {
                // A variable read straight by the cast carries the cast's own op. The C can print one
                // op twice, as in x * x, and then each print is a cast of it.
                if (CONVERSIONS.contains(op.getOpcode()) && e - s <= 3) {
                    return op;
                }
                mine.add(op);
            }
        }
        frontier.addAll(mine);
        // The C can also print a longer expression twice, as the test and the assignment of
        // pfVar2 = (float *)param_1[0x29] in FUN_8c07db84: a cast elsewhere already owns the op.
        PcodeOp printedTwice = null;
        for (int depth = 0; depth < 3 && !frontier.isEmpty(); depth++) {
            List<PcodeOp> next = new ArrayList<>();
            for (PcodeOp op : frontier) {
                Varnode out = op.getOutput();
                if (out == null) {
                    continue;
                }
                Iterator<PcodeOp> it = out.getDescendants();
                while (it.hasNext()) {
                    PcodeOp d = it.next();
                    if (mine.contains(d) || inside.contains(d)) {
                        continue;
                    }
                    // A conversion whose result goes back into the operand is part of it, such as
                    // the extension of a loaded byte that an addition reads: the cast is further on.
                    if (CONVERSIONS.contains(d.getOpcode()) && !feeds(d, mine) && (toPointer || !isPointerCast(d))) {
                        if (!owned.containsValue(d)) {
                            return d;
                        }
                        if (printedTwice == null) {
                            printedTwice = d;
                        }
                        continue;
                    }
                    if (d.getOpcode() == PcodeOp.LOAD || d.getOpcode() == PcodeOp.PTRSUB
                            || d.getOpcode() == PcodeOp.PTRADD) {
                        next.add(d);
                    }
                }
            }
            mine.addAll(next);
            frontier = next;
        }
        return printedTwice;
    }

    // Whether op's result is read by one of the ops in the set.
    private static boolean feeds(PcodeOp op, Set<PcodeOp> ops) {
        Varnode out = op.getOutput();
        if (out == null) {
            return false;
        }
        Iterator<PcodeOp> it = out.getDescendants();
        while (it.hasNext()) {
            if (ops.contains(it.next())) {
                return true;
            }
        }
        return false;
    }

    // A variable read in part, at offset 0: Ghidra prints the piece as a cast of the whole.
    private static boolean isPartial(List<ClangToken> t, int s, int e) {
        int k = skipBlank(t, s);
        if (k >= e || skipBlank(t, k + 1) < e || !(t.get(k) instanceof ClangVariableToken v)) {
            return false;
        }
        Varnode vn = v.getVarnode();
        if (vn == null || vn.getHigh() == null) {
            return false;
        }
        HighSymbol sym = vn.getHigh().getSymbol();
        return vn.getHigh().getOffset() >= 0 || (sym != null && vn.getSize() < sym.getSize());
    }

    // An integer constant cast to a float type: a float the decompiler had only as bits, such as
    // (float)0x4f000000 for 2^31. A conversion of a constant would have been folded.
    private static boolean isFloatConstant(List<ClangToken> t, int open, int close, int s, int e) {
        int k = skipBlank(t, s);
        if (k >= e || skipBlank(t, k + 1) < e || !(t.get(k) instanceof ClangVariableToken v)
                || v.getVarnode() == null || !v.getVarnode().isConstant()
                || !t.get(k).getText().matches("-?(0x[0-9a-fA-F]+|[0-9]+)")) {
            return false;
        }
        for (int j = open + 1; j < close; j++) {
            if (t.get(j) instanceof ClangTypeToken tt && tt.getDataType() != null) {
                DataType dt = tt.getDataType();
                while (dt instanceof TypeDef td) {
                    dt = td.getBaseDataType();
                }
                return dt instanceof AbstractFloatDataType;
            }
        }
        return false;
    }

    // A cast to a float type with no conversion behind it, of a value that float arithmetic reads
    // as it is: a float loaded from a table that Ghidra typed as pointers, fmov.s'd straight into an
    // FPU register. (float)(&PTR_DAT_8c0d17e0)[i] converted the table's 1.8 numerically.
    private static boolean isFloatOfBits(List<ClangToken> t, int open, int close, int s, int e) {
        if (!castsToFloat(t, open, close)) {
            return false;
        }
        List<PcodeOp> frontier = new ArrayList<>();
        for (int k = s; k < e; k++) {
            PcodeOp op = t.get(k).getPcodeOp();
            if (op == null) {
                continue;
            }
            if (op.getOpcode() >= PcodeOp.FLOAT_INT2FLOAT && op.getOpcode() <= PcodeOp.FLOAT_ROUND) {
                return false;  // a conversion
            }
            frontier.add(op);
        }
        // The operand's word comes from a LOAD near its ops (an array element is a PTRADD and a
        // LOAD away from the array's name), and float arithmetic reads that word as it is.
        Set<PcodeOp> seen = new HashSet<>(frontier);
        for (int depth = 0; depth < 3 && !frontier.isEmpty(); depth++) {
            List<PcodeOp> next = new ArrayList<>();
            for (PcodeOp op : frontier) {
                Varnode out = op.getOutput();
                if (out == null) {
                    continue;
                }
                if (op.getOpcode() == PcodeOp.LOAD && readAsFloat(out)) {
                    return true;
                }
                Iterator<PcodeOp> it = out.getDescendants();
                while (it.hasNext()) {
                    PcodeOp d = it.next();
                    if (seen.add(d)) {
                        next.add(d);
                    }
                }
            }
            frontier = next;
        }
        return false;
    }

    private static final Set<Integer> FLOAT_ARITHMETIC = Set.of(PcodeOp.FLOAT_ADD, PcodeOp.FLOAT_SUB,
        PcodeOp.FLOAT_MULT, PcodeOp.FLOAT_DIV, PcodeOp.FLOAT_NEG, PcodeOp.FLOAT_ABS, PcodeOp.FLOAT_SQRT);

    // A cast to an integer type with no conversion behind it, of a value that float arithmetic
    // makes: the float goes into a word that Ghidra typed as an integer, as an fmov.s stores it.
    // FUN_8c0bc01c's param_1[6] = (int)(a * b + c) truncated 15.0125 to 15.
    private static boolean isBitsOfFloat(List<ClangToken> t, int open, int close, int s, int e) {
        if (castsToFloat(t, open, close) || castsToPointer(t, open, close)) {
            return false;
        }
        Set<PcodeOp> ops = new HashSet<>();
        for (int k = s; k < e; k++) {
            if (t.get(k).getPcodeOp() != null) {
                ops.add(t.get(k).getPcodeOp());
            }
        }
        // The operand's value: the op whose result no other op of the operand reads. An ftrc after it
        // is a conversion that the search for the cast's op can miss (FUN_8c093e04, FUN_8c0c5494).
        for (PcodeOp op : ops) {
            if (FLOAT_ARITHMETIC.contains(op.getOpcode()) && !feeds(op, ops)) {
                return !truncated(op.getOutput(), 3);
            }
        }
        return false;
    }

    // Whether a FLOAT_TRUNC reads the value, directly or through copies, casts and phis.
    private static boolean truncated(Varnode v, int depth) {
        if (v == null || depth == 0) {
            return false;
        }
        Iterator<PcodeOp> it = v.getDescendants();
        while (it.hasNext()) {
            PcodeOp d = it.next();
            int code = d.getOpcode();
            if (code >= PcodeOp.FLOAT_TRUNC && code <= PcodeOp.FLOAT_ROUND) {
                return true;
            }
            if ((code == PcodeOp.COPY || code == PcodeOp.CAST || code == PcodeOp.MULTIEQUAL
                    || code == PcodeOp.INDIRECT) && truncated(d.getOutput(), depth - 1)) {
                return true;
            }
        }
        return false;
    }

    // Float arithmetic reads the word, directly or through the decompiler's CAST, and nothing
    // converts it.
    private static boolean readAsFloat(Varnode v) {
        boolean floatUse = false;
        Iterator<PcodeOp> it = v.getDescendants();
        while (it.hasNext()) {
            PcodeOp d = it.next();
            int code = d.getOpcode();
            if (code == PcodeOp.FLOAT_INT2FLOAT || code == PcodeOp.FLOAT_FLOAT2FLOAT) {
                return false;
            }
            if (code == PcodeOp.CAST && d.getOutput() != null && isFloat(d.getOutput())) {
                floatUse |= readAsFloat(d.getOutput());
            }
            floatUse |= code >= PcodeOp.FLOAT_EQUAL && code <= PcodeOp.FLOAT_ROUND;
        }
        return floatUse;
    }

    private static boolean isFloatConversion(PcodeOp op) {
        return op.getOpcode() >= PcodeOp.FLOAT_INT2FLOAT && op.getOpcode() <= PcodeOp.FLOAT_ROUND;
    }

    // (float), not (float *).
    private static boolean castsToFloat(List<ClangToken> t, int open, int close) {
        for (int j = open + 1; j < close; j++) {
            if (t.get(j).getText().equals("*")) {
                return false;
            }
        }
        for (int j = open + 1; j < close; j++) {
            if (t.get(j) instanceof ClangTypeToken tt && tt.getDataType() != null) {
                DataType dt = tt.getDataType();
                while (dt instanceof TypeDef td) {
                    dt = td.getBaseDataType();
                }
                return dt instanceof AbstractFloatDataType;
            }
        }
        return false;
    }

    // A CAST between a float and a non-float, or a SUBPIECE or extension of or to a float: the
    // low word of a register pair is no conversion of a double either.
    private static boolean isPun(PcodeOp op) {
        if (!BIT_MOVES.contains(op.getOpcode()) || op.getOutput() == null) {
            return false;
        }
        boolean in = isFloat(op.getInput(0)), out = isFloat(op.getOutput());
        return op.getOpcode() == PcodeOp.CAST ? in != out : in || out;
    }

    private static boolean isFloat(Varnode v) {
        return v != null && v.getHigh() != null && isFloatType(v.getHigh().getDataType());
    }

    private static boolean isUndefined(Varnode v) {
        if (v == null || v.getHigh() == null) {
            return false;
        }
        DataType dt = v.getHigh().getDataType();
        return dt instanceof Undefined || dt instanceof DefaultDataType;
    }

    private static boolean isFloatType(DataType dt) {
        while (dt instanceof TypeDef td) {
            dt = td.getBaseDataType();
        }
        return dt instanceof AbstractFloatDataType;
    }

    // Where the decompiled function returns its value: "r0:4", "fr0:4", or a pair such as "dr0:8".
    private static String returnStorage(HighFunction hf) {
        ghidra.program.model.listing.VariableStorage s = hf.getFunctionPrototype().getReturnStorage();
        return s == null ? "" : s.toString();
    }

    private static String sidecar(Function f, HighFunction hf, String signature) {
        StringBuilder b = new StringBuilder();
        b.append("{\"name\": ").append(q(f.getName()))
            .append(", \"entry\": ").append(q(f.getEntryPoint().toString()))
            .append(", \"signature\": ").append(q(signature))
            .append(", \"stack_purge\": ").append(f.getStackPurgeSize())
            .append(", \"return_storage\": ").append(q(returnStorage(hf)))
            .append(",\n \"locals\": [");
        boolean first = true;
        java.util.Iterator<HighSymbol> it = hf.getLocalSymbolMap().getSymbols();
        while (it.hasNext()) {
            HighSymbol s = it.next();
            b.append(first ? "\n  " : ",\n  ").append(symbol(s));
            first = false;
        }
        // Call targets by address: two functions can share a name (two kmWaitVBlank in this game).
        b.append("],\n \"calls\": [");
        first = true;
        for (Function g : f.getCalledFunctions(null)) {
            b.append(first ? "" : ", ").append("{\"name\": ").append(q(g.getName()))
                .append(", \"entry\": ").append(q(g.getEntryPoint().toString())).append("}");
            first = false;
        }
        b.append("],\n \"globals\": [");
        first = true;
        it = hf.getGlobalSymbolMap().getSymbols();
        while (it.hasNext()) {
            HighSymbol s = it.next();
            b.append(first ? "\n  " : ",\n  ").append(symbol(s));
            first = false;
        }
        return b.append("]}\n").toString();
    }

    private static String symbol(HighSymbol s) {
        // The type the C prints is the variable's in this function, which can differ from the
        // type stored with a global's symbol.
        DataType dt = s.getHighVariable() != null ? s.getHighVariable().getDataType()
                : s.getDataType();
        VariableStorage st = s.getStorage();
        StringBuilder b = new StringBuilder();
        DataType st0 = s.getDataType();
        b.append("{\"name\": ").append(q(s.getName()))
            .append(", \"type\": ").append(q(dt == null ? "undefined" : dt.getDisplayName()))
            .append(", \"symtype\": ").append(q(st0 == null ? "undefined" : st0.getDisplayName()))
            .append(", \"size\": ").append(s.getSize())
            .append(", \"param\": ").append(s.isParameter() ? s.getCategoryIndex() : -1);
        if (st != null && st.isStackStorage()) {
            b.append(", \"stack\": ").append(st.getStackOffset());
        }
        else if (st != null && st.isMemoryStorage()) {
            b.append(", \"address\": ").append(q(st.getMinAddress().toString()));
            // The C can print another of the address's names than the symbol's (_ctype for
            // LAB_8c0cc8a8), so all of them are listed.
            b.append(", \"aliases\": [");
            boolean first = true;
            for (ghidra.program.model.symbol.Symbol sym : s.getProgram().getSymbolTable()
                    .getSymbols(st.getMinAddress())) {
                b.append(first ? "" : ", ").append(q(sym.getName()));
                first = false;
            }
            b.append("]");
        }
        else if (st != null && st.isRegisterStorage()) {
            b.append(", \"register\": ").append(q(st.getRegister().getName()));
        }
        if (st != null) {
            b.append(", \"storage\": ").append(q(st.toString()));
        }
        return b.append("}").toString();
    }

    private static String q(String s) {
        if (s == null) {
            return "null";
        }
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            if (c == '"' || c == '\\') {
                b.append('\\').append(c);
            }
            else if (c < 0x20) {
                b.append(String.format("\\u%04x", (int) c));
            }
            else {
                b.append(c);
            }
        }
        return b.append('"').toString();
    }

    private static void write(File f, String text) throws Exception {
        try (PrintWriter w = new PrintWriter(f)) {
            w.print(text);
        }
    }
}
