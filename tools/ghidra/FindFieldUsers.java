// Prints non-stack instruction references to exact structure offsets.
// Usage: analyzeHeadless ... -postScript FindFieldUsers.java 180001000 1821fffff f8 167
// @category Apex Editor

import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

public class FindFieldUsers extends GhidraScript {
    private static final Pattern FIELD = Pattern.compile("\\[([a-z0-9]+) \\+ 0x([0-9a-f]+)\\]", Pattern.CASE_INSENSITIVE);

    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length < 3) throw new IllegalArgumentException("Usage: start end offset [offset...]");
        Address start = toAddr(arguments[0]);
        Address end = toAddr(arguments[1]);
        Set<String> offsets = new HashSet<>();
        for (int index = 2; index < arguments.length; index++) {
            offsets.add(arguments[index].toLowerCase(Locale.ROOT).replace("0x", ""));
        }
        InstructionIterator instructions = currentProgram.getListing().getInstructions(start, true);
        while (instructions.hasNext() && !monitor.isCancelled()) {
            Instruction instruction = instructions.next();
            if (instruction.getAddress().compareTo(end) >= 0) break;
            Matcher matcher = FIELD.matcher(instruction.toString());
            while (matcher.find()) {
                String base = matcher.group(1).toLowerCase(Locale.ROOT);
                String offset = matcher.group(2).toLowerCase(Locale.ROOT);
                if (base.equals("rsp") || base.equals("rbp") || !offsets.contains(offset)) continue;
                Function function = getFunctionContaining(instruction.getAddress());
                println(String.join("|", "APEX_FIELD", offset, instruction.getAddress().toString(),
                    function == null ? "<no-function>" : function.getEntryPoint().toString(),
                    function == null ? "<no-function>" : function.getName(true), instruction.toString()));
            }
        }
    }
}
