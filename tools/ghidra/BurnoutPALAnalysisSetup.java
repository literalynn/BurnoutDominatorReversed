// Restrict analysis to the verified PAL Emotion Engine code section.
// @category BurnoutDominator

import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;

public class BurnoutPALAnalysisSetup extends GhidraScript {
    private static final String ELF_SHA256 = "fe8b4b28e165620a35e55bcf6fb452de16cf7982ea6584ab28dc5f6b4b187301";

    @Override
    public void run() throws Exception {
        if (!ELF_SHA256.equalsIgnoreCase(currentProgram.getExecutableSHA256())) {
            throw new IllegalArgumentException("This analysis setup applies only to the verified SLES_546.27 ELF.");
        }
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        if (text == null || text.getStart().getOffset() != 0x00100000L || text.getSize() != 2878248L) {
            throw new IllegalArgumentException("Unexpected PAL .text geometry.");
        }
        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            if (block.isExecute() && block != text) {
                block.setExecute(false);
                println("Cleared executable permission for data/VU section " + block.getName());
            }
        }
    }
}
