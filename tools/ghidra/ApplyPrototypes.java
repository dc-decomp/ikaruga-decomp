// Post-analysis: apply the hand-written prototypes in a file, one C declaration per line, to the
// functions of those names. They are user-defined, so later passes leave them alone.
// Usage (headless): -postScript ApplyPrototypes.java <prototypes.txt>
//@category Dreamcast

import java.nio.file.Files;
import java.nio.file.Path;

import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParser;
import ghidra.program.model.data.FunctionDefinition;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ApplyPrototypes extends GhidraScript {

    @Override
    public void run() throws Exception {
        int applied = 0, missing = 0, failed = 0;
        for (String line : Files.readAllLines(Path.of(getScriptArgs()[0]))) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) {
                continue;
            }
            CParser parser = new CParser(currentProgram.getDataTypeManager());
            FunctionDefinition fd = null;
            try {
                parser.parse(line);
                fd = (FunctionDefinition) parser.getFunctions().values().stream().findFirst().orElse(null);
            }
            catch (Exception e) {
                println("ApplyPrototypes: " + e.getMessage());
            }
            if (fd == null) {
                println("ApplyPrototypes: cannot parse " + line);
                failed++;
                continue;
            }
            boolean found = false;
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (!f.getName().equals(fd.getName())) {
                    continue;
                }
                found = true;
                if (new ApplyFunctionSignatureCmd(f.getEntryPoint(), fd, SourceType.USER_DEFINED)
                        .applyTo(currentProgram, monitor)) {
                    applied++;
                }
                else {
                    failed++;
                }
            }
            if (!found) {
                missing++;
            }
        }
        println("ApplyPrototypes: " + applied + " applied, " + missing + " names not found, "
                + failed + " failed");
    }
}
