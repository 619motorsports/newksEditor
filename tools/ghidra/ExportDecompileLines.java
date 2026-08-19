// Ghidra headless post-script. Prints a selected one-based decompiler line range.
// @category Apex Editor

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class ExportDecompileLines extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length != 3) throw new IllegalArgumentException("Usage: function-address first-line last-line");
        Address address = currentProgram.getAddressFactory().getAddress(arguments[0]);
        Function function = address == null ? null : getFunctionAt(address);
        if (function == null && address != null) function = getFunctionContaining(address);
        if (function == null) throw new IllegalArgumentException("Function not found: " + arguments[0]);
        int first = Math.max(1, Integer.parseInt(arguments[1]));
        int last = Math.max(first, Integer.parseInt(arguments[2]));

        DecompInterface decompiler = new DecompInterface();
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(false);
        if (!decompiler.openProgram(currentProgram)) throw new IllegalStateException("Could not open program in decompiler");
        try {
            DecompileResults result = decompiler.decompileFunction(function, 60, monitor);
            if (!result.decompileCompleted() || result.getDecompiledFunction() == null) {
                throw new IllegalStateException(result.getErrorMessage());
            }
            String[] lines = result.getDecompiledFunction().getC().split("\\R");
            println("APEX_LINES_START|" + function.getEntryPoint() + "|" + first + "|" + last + "|total=" + lines.length);
            for (int line = first; line <= Math.min(last, lines.length); line++) {
                println(line + "|" + lines[line - 1]);
            }
            println("APEX_LINES_END|" + function.getEntryPoint());
        } finally {
            decompiler.dispose();
        }
    }
}
