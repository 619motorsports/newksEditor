// Ghidra headless post-script. Prints little-endian scalar interpretations at addresses.
// @category Apex Editor

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class DumpScalars extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String argument : getScriptArgs()) {
            Address address = currentProgram.getAddressFactory().getAddress(argument);
            if (address == null) throw new IllegalArgumentException("Invalid address: " + argument);
            byte[] bytes = new byte[8];
            currentProgram.getMemory().getBytes(address, bytes);
            ByteBuffer buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
            int bits = buffer.getInt(0);
            long longBits = buffer.getLong(0);
            println(String.format(
                "APEX_SCALAR|%s|u32=%d|hex32=%08x|f32=%.9g|hex64=%016x|f64=%.17g",
                address, Integer.toUnsignedLong(bits), bits, Float.intBitsToFloat(bits), longBits,
                Double.longBitsToDouble(longBits)));
        }
    }
}
