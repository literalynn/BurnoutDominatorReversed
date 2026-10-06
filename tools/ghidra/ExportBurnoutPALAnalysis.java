// Export true Ghidra functions separately from resumable executable labels.
// @category BurnoutDominator

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

public class ExportBurnoutPALAnalysis extends GhidraScript {
    private static final String ELF_SHA256 = "fe8b4b28e165620a35e55bcf6fb452de16cf7982ea6584ab28dc5f6b4b187301";

    @Override
    public void run() throws Exception {
        if (!ELF_SHA256.equalsIgnoreCase(currentProgram.getExecutableSHA256())) {
            throw new IllegalArgumentException("Unexpected source ELF.");
        }
        String[] arguments = getScriptArgs();
        if (arguments.length != 1) {
            throw new IllegalArgumentException("Pass the output directory.");
        }
        File directory = new File(arguments[0]);
        if (!directory.isDirectory()) {
            throw new IllegalArgumentException("Output directory does not exist.");
        }
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        long codeStart = text.getStart().getOffset();
        long codeEnd = codeStart + text.getSize();
        long functionCount = 0L;
        long excludedCount = 0L;
        boolean hasEntry = false;
        try (PrintWriter writer = new PrintWriter(new File(directory, "functions.actual-ee.csv"), StandardCharsets.UTF_8);
             PrintWriter excluded = new PrintWriter(new File(directory, "functions.actual-excluded.csv"), StandardCharsets.UTF_8)) {
            writer.println("Name,Start,End,Size");
            excluded.println("Name,Start,End,Size");
            FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
            while (functions.hasNext() && !monitor.isCancelled()) {
                Function function = functions.next();
                if (function.getBody().isEmpty()) {
                    continue;
                }
                long start = function.getEntryPoint().getOffset();
                long end = function.getBody().getMaxAddress().getOffset() + 1L;
                boolean inCode = start >= codeStart && start < codeEnd && end > start && end <= codeEnd;
                PrintWriter selected = inCode ? writer : excluded;
                selected.printf("%s,0x%08X,0x%08X,%d%n", function.getName(), start, end, function.getBody().getNumAddresses());
                if (inCode) {
                    ++functionCount;
                    hasEntry |= start == 0x00100008L;
                } else {
                    ++excludedCount;
                }
            }
        }
        if (!hasEntry) {
            throw new IllegalStateException("Original entry 0x00100008 is missing.");
        }
        long instructionCount = 0L;
        long instructionBytes = 0L;
        InstructionIterator instructions = currentProgram.getListing().getInstructions(new AddressSet(text.getStart(), text.getEnd()), true);
        while (instructions.hasNext() && !monitor.isCancelled()) {
            Instruction instruction = instructions.next();
            ++instructionCount;
            instructionBytes += instruction.getLength();
        }
        try (PrintWriter writer = new PrintWriter(new File(directory, "ghidra-program-summary.json"), StandardCharsets.UTF_8)) {
            writer.printf("{\n  \"elf_sha256\": \"%s\",\n  \"actual_ee_functions\": %d,\n  \"actual_excluded_functions\": %d,\n  \"text_instruction_count\": %d,\n  \"text_instruction_bytes\": %d,\n  \"text_total_bytes\": %d,\n  \"has_original_entry\": true\n}\n", ELF_SHA256, functionCount, excludedCount, instructionCount, instructionBytes, text.getSize());
        }
        println("Exported " + functionCount + " actual EE functions; excluded " + excludedCount + " functions outside .text.");
    }
}
