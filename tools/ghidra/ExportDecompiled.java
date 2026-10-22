// Post-analysis: decompile every function into OUT_DIR/<address>.c, and list each one with its size
// and outcome in OUT_DIR/index.tsv. A function the decompiler fails on gets a file holding the error,
// so the index and the directory always list the same functions.
//
// The "Removing unreachable block" warnings are dropped. With the FPU mode pinned, they name the
// other-width path of each fmov and say nothing about the function.
// Next to each <address>.c it writes <address>.json: the prototype, every local and parameter with
// its storage and type, and every global the function uses with the type the decompiler gave it
// there. tools/decomp/convert.py needs these to turn the C into code that compiles.
// Usage (headless): -postScript ExportDecompiled.java <out_dir> [timeout_seconds]
//@category Dreamcast
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Pattern;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.parallel.DecompileConfigurer;
import ghidra.app.decompiler.parallel.DecompilerCallback;
import ghidra.app.decompiler.parallel.ParallelDecompiler;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.DataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.VariableStorage;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.HighSymbol;
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
                    String c = UNREACHABLE.matcher(r.getDecompiledFunction().getC()).replaceAll("");
                    write(new File(out, addr + ".c"), header + c.replaceFirst("^\\n+", "\n"));
                    write(new File(out, addr + ".json"),
                        sidecar(f, r.getHighFunction(), r.getDecompiledFunction().getSignature()));
                    return new String[] { addr, f.getName(), size, "ok" };
                }
                String err = String.valueOf(r.getErrorMessage()).trim().replace('\n', ' ');
                write(new File(out, addr + ".c"), header + "// decompile failed: " + err + "\n");
                return new String[] { addr, f.getName(), size, "failed: " + err };
            }
        };
        cb.setTimeout(timeout);

        List<Function> fns = new ArrayList<>();
        currentProgram.getFunctionManager().getFunctions(true).forEach(fns::add);
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

    private static String sidecar(Function f, HighFunction hf, String signature) {
        StringBuilder b = new StringBuilder();
        b.append("{\"name\": ").append(q(f.getName()))
            .append(", \"entry\": ").append(q(f.getEntryPoint().toString()))
            .append(", \"signature\": ").append(q(signature))
            .append(", \"stack_purge\": ").append(f.getStackPurgeSize())
            .append(",\n \"locals\": [");
        boolean first = true;
        java.util.Iterator<HighSymbol> it = hf.getLocalSymbolMap().getSymbols();
        while (it.hasNext()) {
            HighSymbol s = it.next();
            b.append(first ? "\n  " : ",\n  ").append(symbol(s));
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
        DataType dt = s.getDataType();
        VariableStorage st = s.getStorage();
        StringBuilder b = new StringBuilder();
        b.append("{\"name\": ").append(q(s.getName()))
            .append(", \"type\": ").append(q(dt == null ? "undefined" : dt.getDisplayName()))
            .append(", \"size\": ").append(s.getSize())
            .append(", \"param\": ").append(s.isParameter() ? s.getCategoryIndex() : -1);
        if (st != null && st.isStackStorage()) {
            b.append(", \"stack\": ").append(st.getStackOffset());
        }
        else if (st != null && st.isMemoryStorage()) {
            b.append(", \"address\": ").append(q(st.getMinAddress().toString()));
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
