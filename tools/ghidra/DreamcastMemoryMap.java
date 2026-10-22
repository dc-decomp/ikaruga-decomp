// Pre-analysis: give a raw 1ST_READ.BIN import the rest of the Dreamcast address space, so references
// to BSS, the heap and the hardware resolve to blocks instead of dangling addresses.
//
// Hardware blocks are uninitialised and volatile: the decompiler must not fold away a register write
// it cannot see a read of. Addresses are the P2 (uncached) views the SDK uses.
//@category Dreamcast
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class DreamcastMemoryMap extends GhidraScript {

    private void block(String name, long start, long end, boolean hw) throws Exception {
        Memory mem = currentProgram.getMemory();
        Address a = toAddr(start);
        if (mem.getBlock(a) != null) {
            return;
        }
        MemoryBlock b = mem.createUninitializedBlock(name, a, end - start + 1, false);
        b.setRead(true);
        b.setWrite(true);
        b.setExecute(!hw);
        b.setVolatile(hw);
    }

    @Override
    public void run() throws Exception {
        MemoryBlock image = currentProgram.getMemory().getBlock(currentProgram.getMinAddress());
        image.setName("1st_read");
        image.setRead(true);
        image.setWrite(true);
        image.setExecute(true);
        long imageEnd = image.getEnd().getOffset();

        block("sysram", 0x8C000000L, 0x8C00FFFFL, false);
        block("ram", imageEnd + 1, 0x8CFFFFFFL, false);

        block("bios", 0xA0000000L, 0xA01FFFFFL, true);
        block("flash", 0xA0200000L, 0xA021FFFFL, true);
        block("holly_sb", 0xA05F6800L, 0xA05F7FFFL, true);
        block("pvr_regs", 0xA05F8000L, 0xA05F9FFFL, true);
        block("aica_regs", 0xA0700000L, 0xA0710FFFL, true);
        block("aica_ram", 0xA0800000L, 0xA09FFFFFL, true);
        block("vram64", 0xA4000000L, 0xA47FFFFFL, true);
        block("vram32", 0xA5000000L, 0xA57FFFFFL, true);
        block("ta_fifo", 0x10000000L, 0x13FFFFFFL, true);
        block("store_queue", 0xE0000000L, 0xE3FFFFFFL, true);
        block("sh4_regs", 0xFF000000L, 0xFFFFFFFFL, true);

        println("DreamcastMemoryMap: image ends at 0x" + Long.toHexString(imageEnd));
    }
}
