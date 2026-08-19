// Ghidra headless post-script. Prints functions that reference selected strings.
// @category Apex Editor

import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.nio.charset.StandardCharsets;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;

public class ExportStringXrefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length == 0) {
            arguments = new String[] { "LIGHT_SERIES", "CLUSTER_THRESHOLD", "SHADER_REPLACEMENT" };
        }
        String[] needles = new String[arguments.length];
        for (int i = 0; i < arguments.length; i++) needles[i] = arguments[i].toLowerCase(Locale.ROOT);

        Set<String> seen = new HashSet<>();
        for (String needle : arguments) {
            scanBytes(seen, needle, needle.getBytes(StandardCharsets.US_ASCII), "ascii");
            byte[] ascii = needle.getBytes(StandardCharsets.US_ASCII);
            byte[] wide = new byte[ascii.length * 2];
            for (int i = 0; i < ascii.length; i++) wide[i * 2] = ascii[i];
            scanBytes(seen, needle, wide, "utf16le");
        }

        DataIterator definedData = currentProgram.getListing().getDefinedData(true);
        while (definedData.hasNext() && !monitor.isCancelled()) {
            Data data = definedData.next();
            Object value = data.getValue();
            if (!(value instanceof String)) continue;
            String text = ((String) value).replace('|', ' ');
            String lowered = text.toLowerCase(Locale.ROOT);
            String matched = null;
            for (String needle : needles) {
                if (lowered.contains(needle)) { matched = needle; break; }
            }
            if (matched == null) continue;

            Reference[] references = getReferencesTo(data.getAddress());
            if (references.length == 0) {
                emit(seen, matched, data.getAddress().toString(), text, "", "");
                continue;
            }
            for (Reference reference : references) {
                Function function = getFunctionContaining(reference.getFromAddress());
                emit(seen, matched, data.getAddress().toString(), text,
                    reference.getFromAddress().toString(), function == null ? "<no-function>" : function.getName(true));
            }
        }
    }

    private void scanBytes(Set<String> seen, String needle, byte[] bytes, String encoding) throws Exception {
        Memory memory = currentProgram.getMemory();
        Address cursor = memory.getMinAddress();
        while (cursor != null && !monitor.isCancelled()) {
            Address address = memory.findBytes(cursor, bytes, null, true, monitor);
            if (address == null) break;
            Reference[] references = getReferencesTo(address);
            if (references.length == 0) {
                emit(seen, needle.toLowerCase(Locale.ROOT), address.toString(), needle + " [" + encoding + "]", "", "");
            } else {
                for (Reference reference : references) {
                    Function function = getFunctionContaining(reference.getFromAddress());
                    emit(seen, needle.toLowerCase(Locale.ROOT), address.toString(), needle + " [" + encoding + "]",
                        reference.getFromAddress().toString(), function == null ? "<no-function>" : function.getName(true));
                }
            }
            cursor = address.add(1);
        }
    }

    private void emit(Set<String> seen, String needle, String address, String text, String reference, String function) {
        String row = String.join("|", "APEX_XREF", needle, address, text, reference, function);
        if (seen.add(row)) println(row);
    }
}
