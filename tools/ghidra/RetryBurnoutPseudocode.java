// Retry only timed-out pseudocode exports and preserve all first-pass evidence.
// @category BurnoutDominator

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import java.io.BufferedWriter;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;

public class RetryBurnoutPseudocode extends GhidraScript {
    private static final String ELF_SHA256 = "fe8b4b28e165620a35e55bcf6fb452de16cf7982ea6584ab28dc5f6b4b187301";
    private static final int TIMEOUT_SECONDS = 60;

    private JsonObject attempt(JsonObject record, int number, String pass) {
        JsonObject attempt = new JsonObject();
        attempt.addProperty("number", number);
        attempt.addProperty("pass", pass);
        for (String key : new String[]{"status", "timeout_seconds", "duration_ms", "warning_or_error"}) {
            attempt.add(key, record.get(key).deepCopy());
        }
        return attempt;
    }

    @Override
    public void run() throws Exception {
        if (!ELF_SHA256.equalsIgnoreCase(currentProgram.getExecutableSHA256())) throw new IllegalArgumentException("Unexpected source ELF.");
        String[] arguments = getScriptArgs();
        if (arguments.length != 1) throw new IllegalArgumentException("Pass the existing pseudocode directory.");
        Path output = new File(arguments[0]).getCanonicalFile().toPath();
        Path manifestPath = output.resolve("manifest.json");
        JsonObject manifest = JsonParser.parseString(Files.readString(manifestPath, StandardCharsets.UTF_8)).getAsJsonObject();
        if (!ELF_SHA256.equalsIgnoreCase(manifest.get("source_elf_sha256").getAsString())) throw new IllegalArgumentException("Manifest source mismatch.");
        if (manifest.has("retry_pass")) throw new IllegalArgumentException("This manifest already has a recorded retry pass.");
        Path firstManifest = output.resolve("manifest.first-pass.json");
        Path firstJournal = output.resolve("manifest.first-pass.jsonl");
        if (!Files.exists(firstManifest)) Files.copy(manifestPath, firstManifest);
        if (!Files.exists(firstJournal)) Files.copy(output.resolve("manifest.jsonl"), firstJournal);
        manifest.add("first_pass_status_counts", manifest.get("status_counts").deepCopy());
        manifest.add("first_pass_duration_ms", manifest.get("duration_ms").deepCopy());
        manifest.add("first_pass_finished_utc", manifest.get("finished_utc").deepCopy());
        JsonArray functions = manifest.getAsJsonArray("functions");
        for (JsonElement element : functions) {
            JsonObject item = element.getAsJsonObject();
            JsonArray attempts = new JsonArray();
            attempts.add(attempt(item, 1, "initial_parallel_export"));
            item.add("attempts", attempts);
            item.addProperty("total_attempt_duration_ms", item.get("duration_ms").getAsLong());
        }
        String retryStarted = Instant.now().toString();
        long runStarted = System.nanoTime();
        JsonArray retriedEntries = new JsonArray();
        JsonObject retryCounts = new JsonObject();
        DecompInterface decompiler = new DecompInterface();
        DecompileOptions options = new DecompileOptions();
        options.grabFromProgram(currentProgram);
        options.setDefaultTimeout(TIMEOUT_SECONDS);
        decompiler.setOptions(options);
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(false);
        decompiler.setSimplificationStyle("decompile");
        if (!decompiler.openProgram(currentProgram)) throw new IllegalStateException("Unable to open the saved program in the decompiler.");
        try (BufferedWriter retryJournal = Files.newBufferedWriter(output.resolve("retry-attempts.jsonl"), StandardCharsets.UTF_8)) {
            for (JsonElement element : functions) {
                JsonObject item = element.getAsJsonObject();
                if (!item.get("status").getAsString().equals("timeout")) continue;
                String entry = item.get("entry").getAsString();
                long address = Long.parseLong(entry.substring(2), 16);
                Function function = currentProgram.getFunctionManager().getFunctionAt(toAddr(address));
                if (function == null) throw new IllegalStateException("Saved function is missing: " + entry);
                retriedEntries.add(entry);
                println("Retrying " + entry + " with a 60 second timeout.");
                long functionStarted = System.nanoTime();
                String status;
                String error = "";
                String code = "";
                try {
                    DecompileResults result = decompiler.decompileFunction(function, TIMEOUT_SECONDS, monitor);
                    error = result.getErrorMessage();
                    if (result.isTimedOut()) status = "timeout";
                    else if (result.isCancelled()) status = "cancelled";
                    else if (result.failedToStart()) status = "failed_to_start";
                    else if (result.decompileCompleted() && result.getDecompiledFunction() != null) {
                        code = result.getDecompiledFunction().getC();
                        status = code == null || code.isBlank() ? "failed_no_pseudocode" : "success";
                    } else status = "failed";
                } catch (Exception exception) {
                    status = "failed";
                    error = exception.toString();
                }
                long duration = (System.nanoTime() - functionStarted) / 1000000L;
                item.addProperty("status", status);
                item.addProperty("timeout_seconds", TIMEOUT_SECONDS);
                item.addProperty("duration_ms", duration);
                item.addProperty("timed_out", status.equals("timeout"));
                item.addProperty("warning_or_error", error == null ? "" : error);
                JsonArray warningComments = new JsonArray();
                if (code != null) code.lines().filter(line -> line.contains("WARNING:")).map(String::trim).forEach(warningComments::add);
                item.add("pseudocode_warning_comments", warningComments);
                item.addProperty("total_attempt_duration_ms", item.get("total_attempt_duration_ms").getAsLong() + duration);
                JsonObject retryAttempt = attempt(item, 2, "timeout_retry");
                retryAttempt.addProperty("finished_utc", Instant.now().toString());
                item.getAsJsonArray("attempts").add(retryAttempt);
                JsonObject audit = new JsonObject();
                audit.addProperty("entry", entry);
                audit.add("attempt", retryAttempt.deepCopy());
                retryJournal.write(new Gson().toJson(audit));
                retryJournal.newLine();
                retryJournal.flush();
                Path pseudocode = output.resolve(item.get("output").getAsString()).normalize();
                if (!pseudocode.startsWith(output)) throw new IllegalArgumentException("Output filename escaped the pseudocode directory.");
                String header = "/*\n * GHIDRA DECOMPILER PSEUDOCODE.\n * Not recovered original source; not directly compilable.\n * ELF SHA-256: " + ELF_SHA256 + "\n * Function: " + function.getName().replace("*/", "* /") + " at " + entry + "\n * Export status: " + status + "; retry attempt 2 with 60 second limit\n */\n\n";
                if (code == null || code.isBlank()) header += "/* No pseudocode was produced. See manifest.json for both attempts. */\n";
                Files.writeString(pseudocode, header + (code == null ? "" : code), StandardCharsets.UTF_8);
                retryCounts.addProperty(status, retryCounts.has(status) ? retryCounts.get(status).getAsInt() + 1 : 1);
                println("Retry result: " + entry + " " + status + " in " + duration + " ms.");
            }
        } finally {
            decompiler.dispose();
        }
        JsonObject counts = new JsonObject();
        for (JsonElement element : functions) {
            String status = element.getAsJsonObject().get("status").getAsString();
            counts.addProperty(status, counts.has(status) ? counts.get(status).getAsInt() + 1 : 1);
        }
        long retryDuration = (System.nanoTime() - runStarted) / 1000000L;
        JsonObject retryPass = new JsonObject();
        retryPass.addProperty("started_utc", retryStarted);
        retryPass.addProperty("finished_utc", Instant.now().toString());
        retryPass.addProperty("per_function_timeout_seconds", TIMEOUT_SECONDS);
        retryPass.addProperty("duration_ms", retryDuration);
        retryPass.add("entries", retriedEntries);
        retryPass.add("status_counts", retryCounts);
        manifest.add("retry_pass", retryPass);
        manifest.add("status_counts", counts);
        manifest.addProperty("all_pseudocode_exported", counts.has("success") && counts.get("success").getAsInt() == functions.size());
        manifest.addProperty("duration_ms", manifest.get("first_pass_duration_ms").getAsLong() + retryDuration);
        manifest.addProperty("finished_utc", Instant.now().toString());
        long warningFunctions = 0L;
        for (JsonElement element : functions) {
            JsonObject item = element.getAsJsonObject();
            if (item.has("pseudocode_warning_comments") && !item.getAsJsonArray("pseudocode_warning_comments").isEmpty()) ++warningFunctions;
        }
        manifest.addProperty("functions_with_warning_comments", warningFunctions);
        Gson json = new GsonBuilder().disableHtmlEscaping().create();
        Gson pretty = new GsonBuilder().disableHtmlEscaping().setPrettyPrinting().create();
        Files.writeString(manifestPath, pretty.toJson(manifest) + "\n", StandardCharsets.UTF_8);
        try (BufferedWriter writer = Files.newBufferedWriter(output.resolve("manifest.jsonl"), StandardCharsets.UTF_8)) {
            for (JsonElement element : functions) {
                writer.write(json.toJson(element));
                writer.newLine();
            }
        }
        println("Merged retry results: " + counts);
    }
}
