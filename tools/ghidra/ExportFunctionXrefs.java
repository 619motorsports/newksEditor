// Ghidra headless post-script. Prints native references to selected function entry points.
// @category Apex Editor

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class ExportFunctionXrefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length == 0) throw new IllegalArgumentException("Usage: function-address [function-address...]");
        for (String argument : arguments) {
            Address address = currentProgram.getAddressFactory().getAddress(argument);
            Function target = address == null ? null : getFunctionAt(address);
            if (target == null && address != null) target = getFunctionContaining(address);
            if (target == null) throw new IllegalArgumentException("Function not found: " + argument);
            for (Reference reference : getReferencesTo(target.getEntryPoint())) {
                Function caller = getFunctionContaining(reference.getFromAddress());
                println(String.join("|", "APEX_FUNCTION_XREF", target.getEntryPoint().toString(),
                    target.getName(true), reference.getFromAddress().toString(),
                    caller == null ? "<no-function>" : caller.getEntryPoint().toString(),
                    caller == null ? "<no-function>" : caller.getName(true), reference.getReferenceType().toString()));
            }
        }
    }
}
