// Ghidra headless post-script. Decompiles selected function entry addresses.
// @category Apex Editor

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class ExportDecompile extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface decompiler = new DecompInterface();
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(false);
        if (!decompiler.openProgram(currentProgram)) throw new IllegalStateException("Could not open program in decompiler");
        try {
            for (String argument : getScriptArgs()) {
                Address address = currentProgram.getAddressFactory().getAddress(argument);
                Function function = address == null ? null : getFunctionAt(address);
                if (function == null && address != null) function = getFunctionContaining(address);
                if (function == null) {
                    println("APEX_DECOMPILE_MISSING|" + argument);
                    continue;
                }
                DecompileResults result = decompiler.decompileFunction(function, 60, monitor);
                println("APEX_DECOMPILE_START|" + function.getEntryPoint() + "|" + function.getName(true));
                if (result.decompileCompleted() && result.getDecompiledFunction() != null) {
                    println(result.getDecompiledFunction().getC());
                } else {
                    println("APEX_DECOMPILE_ERROR|" + result.getErrorMessage());
                }
                println("APEX_DECOMPILE_END|" + function.getEntryPoint());
            }
        } finally {
            decompiler.dispose();
        }
    }
}
