// Post-analysis: give Ghidra the switch tables that dream-recomp's discovery recovered.
//
// Ghidra misses SHC's tables when a case target was seeded as a function before its dispatcher was
// analysed: the dispatch then decompiles as a call through the table, and each case becomes a
// function that reads the dispatcher's registers as unaff_ inputs. This removes those case functions
// (unless something calls them), writes the table as a jump-table override on the dispatcher and
// grows the dispatcher's body over its cases.
//
// The file holds one table per line: the jump address, then the case targets, all in hex.
// Usage (headless): -postScript ApplySwitchTables.java <switch-tables.txt>
//@category Dreamcast

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.pcode.JumpTable;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

public class ApplySwitchTables extends GhidraScript {

    @Override
    public void run() throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        int applied = 0, removed = 0, kept = 0, noHost = 0;
        for (String line : Files.readAllLines(Path.of(getScriptArgs()[0]))) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) {
                continue;
            }
            String[] f = line.split("\\s+");
            Address site = toAddr(Long.parseLong(f[0].replace("0x", ""), 16));
            List<Address> targets = new ArrayList<>();
            for (int i = 1; i < f.length; i++) {
                Address t = toAddr(Long.parseLong(f[i].replace("0x", ""), 16));
                if (!targets.contains(t)) {
                    targets.add(t);
                }
            }
            Function host = fm.getFunctionContaining(site);
            for (Address t : targets) {
                Function g = fm.getFunctionAt(t);
                if (g == null || g == host) {
                    continue;
                }
                if (isCalled(t)) {
                    kept++;
                    continue;
                }
                fm.removeFunction(t);
                removed++;
            }
            if (host == null) {
                noHost++;
                println("ApplySwitchTables: no function contains the jump at " + site);
                continue;
            }
            for (Address t : targets) {
                if (getInstructionAt(t) == null) {
                    disassemble(t);
                }
            }
            Instruction jump = getInstructionAt(site);
            for (Address t : targets) {
                jump.addOperandReference(0, t, RefType.COMPUTED_JUMP, SourceType.USER_DEFINED);
            }
            new JumpTable(site, new ArrayList<>(targets), true, 0).writeOverride(host);
            CreateFunctionCmd.fixupFunctionBody(currentProgram, host, monitor);
            applied++;
        }
        println("ApplySwitchTables: " + applied + " tables applied, " + removed
                + " case functions removed, " + kept + " kept because they are called, " + noHost
                + " without a dispatcher function");
    }

    private boolean isCalled(Address a) {
        for (Reference r : getReferencesTo(a)) {
            if (r.getReferenceType().isCall()) {
                return true;
            }
        }
        return false;
    }
}
