// Pre-analysis: analyzers that mislead on this binary are turned off.
//
// "Non-Returning Functions - Discovered" marked enemyInit as a function that never returns,
// though it ends in rts. Its 16 callers then lost the code after the call: enemyModel3Create returned
// before it set an object's update callbacks, and the game called through garbage.
//
// Usage (headless): -preScript AnalysisOptions.java
//@category Dreamcast
import ghidra.app.script.GhidraScript;

public class AnalysisOptions extends GhidraScript {
    @Override
    public void run() throws Exception {
        setAnalysisOption(currentProgram, "Non-Returning Functions - Discovered", "false");
        println("AnalysisOptions: non-returning function discovery off");
    }
}
