// Ghidra headless post-script. Prints references to arbitrary addresses.
// @category Apex Editor

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class ExportAddressXrefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String argument : getScriptArgs()) {
            Address address = currentProgram.getAddressFactory().getAddress(argument);
            if (address == null) throw new IllegalArgumentException("Invalid address: " + argument);
            ReferenceIterator references = currentProgram.getReferenceManager().getReferencesTo(address);
            while (references.hasNext()) {
                Reference reference = references.next();
                Function function = getFunctionContaining(reference.getFromAddress());
                println("APEX_ADDRESS_XREF|" + address + "|" + reference.getFromAddress() + "|" +
                    reference.getReferenceType() + "|" + (function == null ? "" : function.getName()) + "|" +
                    (function == null ? "" : function.getEntryPoint()));
            }
        }
    }
}
