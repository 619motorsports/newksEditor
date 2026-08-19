// Ghidra headless post-script. Prints decompiler lines around selected address tokens.
// @category Apex Editor

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class ExportDecompileSnippets extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length < 2) throw new IllegalArgumentException("Usage: function-address token [token...]");
        Address address = currentProgram.getAddressFactory().getAddress(arguments[0]);
        Function function = address == null ? null : getFunctionAt(address);
        if (function == null && address != null) function = getFunctionContaining(address);
        if (function == null) throw new IllegalArgumentException("Function not found: " + arguments[0]);

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
            for (int tokenIndex = 1; tokenIndex < arguments.length; tokenIndex++) {
                String token = arguments[tokenIndex].toLowerCase();
                for (int line = 0; line < lines.length; line++) {
                    if (!lines[line].toLowerCase().contains(token)) continue;
                    println("APEX_SNIPPET_START|" + function.getEntryPoint() + "|" + arguments[tokenIndex] + "|line=" + (line + 1));
                    for (int context = Math.max(0, line - 5); context <= Math.min(lines.length - 1, line + 8); context++) {
                        println(lines[context]);
                    }
                    println("APEX_SNIPPET_END|" + function.getEntryPoint() + "|" + arguments[tokenIndex]);
                }
            }
        } finally {
            decompiler.dispose();
        }
    }
}
