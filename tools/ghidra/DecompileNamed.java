// Decompile functions whose qualified names contain any supplied argument.
// Usage: analyzeHeadless ... -postScript DecompileNamed.java CameraTrack::loadSet CameraTrack::update
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class DecompileNamed extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] requested = getScriptArgs();
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try {
            FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
            while (functions.hasNext()) {
                Function function = functions.next();
                String qualified = function.getName(true);
                boolean selected = requested.length == 0;
                for (String name : requested) {
                    boolean contains = name.startsWith("*");
                    String query = contains ? name.substring(1) : name;
                    if (contains ? qualified.contains(query) : qualified.equals(query)) {
                        selected = true;
                        break;
                    }
                }
                if (!selected) continue;
                println("\n===== " + qualified + " @ " + function.getEntryPoint() + " =====");
                DecompileResults result = decompiler.decompileFunction(function, 120, monitor);
                if (!result.decompileCompleted()) {
                    println("Decompiler error: " + result.getErrorMessage());
                    continue;
                }
                println(result.getDecompiledFunction().getC());
            }
        } finally {
            decompiler.dispose();
        }
    }
}
