// Post-analysis: commit the parameters and return value the decompiler infers for every function
// whose prototype did not come from a header or by hand.
//
// Without a committed prototype the decompiler guesses the arguments at each call site on its own,
// so one callee is called with different argument counts from different places, and a callee reads
// the registers it was not given as in_ inputs. Functions are visited callees first, so a caller is
// decompiled against the prototypes of what it calls.
//
// Usage (headless): -postScript CommitPrototypes.java
//@category Dreamcast

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.HighFunctionDBUtil;
import ghidra.program.model.pcode.HighFunctionDBUtil.ReturnCommitOption;
import ghidra.program.model.symbol.SourceType;

public class CommitPrototypes extends GhidraScript {

    @Override
    public void run() throws Exception {
        List<Function> order = new ArrayList<>();
        Set<Function> seen = new HashSet<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            postOrder(f, seen, order);
        }
        DecompInterface di = new DecompInterface();
        di.setOptions(new DecompileOptions());
        di.setSimplificationStyle("decompile");
        di.openProgram(currentProgram);
        int committed = 0, skipped = 0, failed = 0;
        for (Function f : order) {
            if (monitor.isCancelled()) {
                break;
            }
            SourceType src = f.getSignatureSource();
            if (src == SourceType.IMPORTED || src == SourceType.USER_DEFINED) {
                skipped++;
                continue;
            }
            DecompileResults res = di.decompileFunction(f, 60, monitor);
            HighFunction hf = res.getHighFunction();
            if (hf == null) {
                failed++;
                continue;
            }
            try {
                HighFunctionDBUtil.commitParamsToDatabase(hf, true, ReturnCommitOption.COMMIT_NO_VOID,
                    SourceType.ANALYSIS);
                committed++;
            }
            catch (Exception e) {
                failed++;
                println("CommitPrototypes: " + f.getName() + ": " + e.getMessage());
            }
        }
        di.dispose();
        println("CommitPrototypes: " + committed + " committed, " + skipped
                + " kept (header or hand-written), " + failed + " failed");
    }

    // Callees first. Call cycles are broken wherever the walk meets a function it has already seen.
    private void postOrder(Function f, Set<Function> seen, List<Function> order) {
        List<Function> stack = new ArrayList<>();
        List<java.util.Iterator<Function>> iters = new ArrayList<>();
        if (!seen.add(f)) {
            return;
        }
        stack.add(f);
        iters.add(f.getCalledFunctions(monitor).iterator());
        while (!stack.isEmpty()) {
            java.util.Iterator<Function> it = iters.get(iters.size() - 1);
            if (it.hasNext()) {
                Function g = it.next();
                if (!g.isThunk() && !g.isExternal() && seen.add(g)) {
                    stack.add(g);
                    iters.add(g.getCalledFunctions(monitor).iterator());
                }
                continue;
            }
            order.add(stack.remove(stack.size() - 1));
            iters.remove(iters.size() - 1);
        }
    }
}
