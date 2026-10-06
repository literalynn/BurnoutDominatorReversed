// Export decompiler pseudocode for every verified PAL EE function, without editing the program.
// @category BurnoutDominator

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.parallel.DecompilerCallback;
import ghidra.app.decompiler.parallel.ParallelDecompiler;
import ghidra.framework.Application;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressRangeIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.util.task.TaskMonitor;
import generic.concurrent.GThreadPool;
import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import java.io.BufferedWriter;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;

public class ExportBurnoutPseudocode extends GhidraScript {
    private static final String ELF_SHA256 = "fe8b4b28e165620a35e55bcf6fb452de16cf7982ea6584ab28dc5f6b4b187301";
    private static final int FUNCTION_TIMEOUT_SECONDS = 10;
    private final Gson json = new GsonBuilder().disableHtmlEscaping().create();
    private final Gson prettyJson = new GsonBuilder().disableHtmlEscaping().setPrettyPrinting().create();
    private final ConcurrentHashMap<Long, Map<String, Object>> records = new ConcurrentHashMap<>();
    private final AtomicInteger completed = new AtomicInteger();
    private final ThreadLocal<Long> started = new ThreadLocal<>();
    private File output;
    private BufferedWriter journal;
    private long deadline;
    private long runStarted;
    private int totalFunctions;

    private String hex(long address) {
        return String.format("0x%08X", address);
    }

    private Map<String, Object> record(Function function, String status, String error, String code) throws Exception {
        long address = function.getEntryPoint().getOffset();
        Map<String, Object> item = new LinkedHashMap<>();
        String safeName = function.getName().replaceAll("[^A-Za-z0-9_.-]", "_");
        if (safeName.length() > 48) safeName = safeName.substring(0, 48);
        String filename = String.format("functions/%08X_%s.pseudocode.c", address, safeName);
        item.put("entry", hex(address));
        item.put("end_exclusive", hex(function.getBody().getMaxAddress().getOffset() + 1L));
        item.put("name", function.getName());
        item.put("body_bytes", function.getBody().getNumAddresses());
        List<Map<String, Object>> ranges = new ArrayList<>();
        AddressRangeIterator rangeIterator = function.getBody().getAddressRanges(true);
        while (rangeIterator.hasNext()) {
            AddressRange range = rangeIterator.next();
            Map<String, Object> bodyRange = new LinkedHashMap<>();
            bodyRange.put("address_space", range.getMinAddress().getAddressSpace().getName());
            bodyRange.put("start", hex(range.getMinAddress().getOffset()));
            bodyRange.put("end_exclusive", hex(range.getMaxAddress().getOffset() + 1L));
            ranges.add(bodyRange);
        }
        item.put("body_ranges", ranges);
        item.put("status", status);
        item.put("timeout_seconds", FUNCTION_TIMEOUT_SECONDS);
        item.put("duration_ms", started.get() == null ? 0L : (System.nanoTime() - started.get()) / 1000000L);
        item.put("timed_out", status.equals("timeout"));
        item.put("warning_or_error", error == null ? "" : error);
        item.put("pseudocode_warning_comments", code == null ? Collections.emptyList() : code.lines().filter(line -> line.contains("WARNING:")).map(String::trim).toList());
        item.put("output", filename);
        String header = "/*\n * GHIDRA DECOMPILER PSEUDOCODE.\n * Not recovered original source; not directly compilable.\n * ELF SHA-256: " + ELF_SHA256 + "\n * Function: " + function.getName().replace("*/", "* /") + " at " + hex(address) + "\n * Export status: " + status + "\n */\n\n";
        if (code == null || code.isBlank()) {
            header += "/* No pseudocode was produced. See manifest.json for the recorded failure. */\n";
        }
        Files.writeString(new File(output, filename).toPath(), header + (code == null ? "" : code), StandardCharsets.UTF_8);
        records.put(address, item);
        synchronized (this) {
            journal.write(json.toJson(item));
            journal.newLine();
            journal.flush();
            int count = completed.incrementAndGet();
            if (count == 1 || count % 250 == 0 || count == totalFunctions) {
                println("Pseudocode progress: " + count + "/" + totalFunctions + "; latest " + hex(address) + " " + status);
            }
        }
        return item;
    }

    @Override
    public void run() throws Exception {
        if (!ELF_SHA256.equalsIgnoreCase(currentProgram.getExecutableSHA256())) {
            throw new IllegalArgumentException("This exporter applies only to the verified SLES_546.27 ELF.");
        }
        String[] arguments = getScriptArgs();
        if (arguments.length < 1 || arguments.length > 2) {
            throw new IllegalArgumentException("Pass the output directory and optional total-budget seconds (default 900).");
        }
        int budgetSeconds = arguments.length == 2 ? Integer.parseInt(arguments[1]) : 900;
        if (budgetSeconds < FUNCTION_TIMEOUT_SECONDS) throw new IllegalArgumentException("Total budget must be at least 10 seconds.");
        output = new File(arguments[0]);
        Files.createDirectories(new File(output, "functions").toPath());
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        if (text == null || text.getStart().getOffset() != 0x00100000L || text.getSize() != 2878248L) {
            throw new IllegalArgumentException("Unexpected PAL .text geometry.");
        }
        long start = text.getStart().getOffset();
        long end = start + text.getSize();
        List<Function> functions = new ArrayList<>();
        FunctionIterator iterator = currentProgram.getFunctionManager().getFunctions(true);
        while (iterator.hasNext()) {
            Function function = iterator.next();
            long address = function.getEntryPoint().getOffset();
            if (!function.getBody().isEmpty() && function.getEntryPoint().getAddressSpace().equals(text.getStart().getAddressSpace()) &&
                address >= start && address < end && function.getBody().getMaxAddress().getOffset() < end) {
                functions.add(function);
            }
        }
        totalFunctions = functions.size();
        if (totalFunctions != 8860) throw new IllegalStateException("Expected 8860 actual EE functions, found " + totalFunctions);
        GThreadPool.getSharedThreadPool("Parallel Decompiler").setMaxThreadCount(4);
        String startedAt = Instant.now().toString();
        runStarted = System.nanoTime();
        deadline = runStarted + budgetSeconds * 1000000000L;
        DecompilerCallback<Map<String, Object>> callback = new DecompilerCallback<Map<String, Object>>(currentProgram, (DecompInterface decompiler) -> {
            DecompileOptions options = new DecompileOptions();
            options.grabFromProgram(currentProgram);
            options.setDefaultTimeout(FUNCTION_TIMEOUT_SECONDS);
            decompiler.setOptions(options);
            decompiler.toggleCCode(true);
            decompiler.toggleSyntaxTree(false);
            decompiler.setSimplificationStyle("decompile");
        }) {
            @Override
            public Map<String, Object> process(Function function, TaskMonitor functionMonitor) throws Exception {
                started.set(System.nanoTime());
                if (System.nanoTime() >= deadline) return record(function, "not_attempted_total_budget", "Total export budget exceeded.", null);
                if (functionMonitor.isCancelled()) return record(function, "cancelled", "Task monitor cancelled.", null);
                try {
                    return super.process(function, functionMonitor);
                } catch (Exception error) {
                    return record(function, "failed", error.toString(), null);
                } finally {
                    started.remove();
                }
            }

            @Override
            public Map<String, Object> process(DecompileResults result, TaskMonitor functionMonitor) throws Exception {
                String status;
                String code = null;
                if (result.isTimedOut()) status = "timeout";
                else if (result.isCancelled()) status = "cancelled";
                else if (result.failedToStart()) status = "failed_to_start";
                else if (result.decompileCompleted() && result.getDecompiledFunction() != null) {
                    status = "success";
                    code = result.getDecompiledFunction().getC();
                    if (code == null || code.isBlank()) status = "failed_no_pseudocode";
                } else status = "failed";
                return record(result.getFunction(), status, result.getErrorMessage(), code);
            }
        };
        callback.setTimeout(FUNCTION_TIMEOUT_SECONDS);
        String runError = "";
        try (BufferedWriter writer = Files.newBufferedWriter(new File(output, "manifest.jsonl").toPath(), StandardCharsets.UTF_8)) {
            journal = writer;
            try {
                ParallelDecompiler.decompileFunctions(callback, functions, monitor);
            } catch (Exception error) {
                runError = error.toString();
            } finally {
                callback.dispose();
            }
            for (Function function : functions) {
                if (!records.containsKey(function.getEntryPoint().getOffset())) {
                    record(function, "not_attempted_run_failure", runError.isEmpty() ? "The parallel queue returned no result." : runError, null);
                }
            }
        }
        List<Map<String, Object>> ordered = new ArrayList<>(records.values());
        ordered.sort(Comparator.comparing(item -> (String)item.get("entry")));
        Map<String, Integer> counts = new LinkedHashMap<>();
        for (Map<String, Object> item : ordered) {
            String status = (String)item.get("status");
            counts.put(status, counts.getOrDefault(status, 0) + 1);
        }
        Map<String, Object> manifest = new LinkedHashMap<>();
        manifest.put("artifact_type", "ghidra_decompiler_pseudocode_not_original_source");
        manifest.put("source_elf_sha256", ELF_SHA256);
        manifest.put("program_name", currentProgram.getName());
        manifest.put("language_id", currentProgram.getLanguageID().toString());
        manifest.put("compiler_spec_id", currentProgram.getCompilerSpec().getCompilerSpecID().toString());
        manifest.put("ghidra_version", Application.getApplicationVersion());
        manifest.put("analysis_policy", "Saved original Ghidra analysis; readonly project; no reimport or reanalysis; actual .text functions only.");
        manifest.put("started_utc", startedAt);
        manifest.put("finished_utc", Instant.now().toString());
        manifest.put("duration_ms", (System.nanoTime() - runStarted) / 1000000L);
        manifest.put("workers", 4);
        manifest.put("per_function_timeout_seconds", FUNCTION_TIMEOUT_SECONDS);
        manifest.put("total_budget_seconds", budgetSeconds);
        manifest.put("expected_functions", totalFunctions);
        manifest.put("recorded_functions", ordered.size());
        manifest.put("status_counts", counts);
        manifest.put("all_pseudocode_exported", counts.getOrDefault("success", 0) == totalFunctions);
        manifest.put("success_definition", "Ghidra completed decompilation and returned nonempty pseudocode; this does not establish semantic correctness or native gameplay.");
        manifest.put("functions_with_warning_comments", ordered.stream().filter(item -> !((List<?>)item.get("pseudocode_warning_comments")).isEmpty()).count());
        manifest.put("run_error", runError);
        manifest.put("functions", ordered);
        Files.writeString(new File(output, "manifest.json").toPath(), prettyJson.toJson(manifest) + "\n", StandardCharsets.UTF_8);
        println("Pseudocode export finished: " + json.toJson(counts));
    }
}
