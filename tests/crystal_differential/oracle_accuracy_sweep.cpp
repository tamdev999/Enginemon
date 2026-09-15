// oracle_accuracy_sweep.cpp -- accuracy conformance suite entry point.
//
// Usage:
//   oracle_accuracy_sweep <rom> <sym> --part-a           [--verbose]
//   oracle_accuracy_sweep <rom> <sym> --part-b [--jobs N] [--verbose]
//   oracle_accuracy_sweep <rom> <sym> --all    [--jobs N] [--verbose]
//
// Worker mode (invoked by coordinator, not by users):
//   oracle_accuracy_sweep <rom> <sym> --part-b-row N
//
// --part-b coordinator design:
//   - Spawns one worker process per ACC raw row (1..13) using CreateProcess.
//   - Workers emit machine-readable lines on stdout; coordinator collects them.
//   - --jobs N controls max parallel workers (default 1).
//   - Results sorted by ACC row for deterministic output regardless of job count.
//   - Fail-closed: any worker crash / harness error / incomplete output fails aggregate.

#include "crystal_differential/oracle_runner.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
// Worker result collected by coordinator
// ============================================================================
struct WorkerResult {
    int acc_raw = 0;

    struct RowEntry {
        int acc_raw      = 0;
        int eva_raw      = 0;
        int crystal      = -1;
        int enginemon    = -1;
    };
    std::vector<RowEntry> entries; // 13 entries per completed worker

    int comparisons = 0;
    int harness     = 0;
    int diffs       = 0;
    bool done       = false;   // saw PARTB_DONE line
    bool ok         = false;   // exit 0 or 1, PARTB_DONE present, harness==0

    std::string error_detail;
};

// ============================================================================
// Build the worker command string for CreateProcess.
// Worker invocation: <exe> <rom> <sym> --part-b-row N
// All args are passed through so rom/sym paths reach runner_main.
// ============================================================================
static std::string build_worker_cmdline(
    const std::string& exe_path,
    const std::vector<std::string>& passthrough_args,
    int acc_raw)
{
    // Quote each token that contains spaces.
    auto quote = [](const std::string& s) -> std::string {
        if(s.find(' ') == std::string::npos && s.find('"') == std::string::npos)
            return s;
        return '"' + s + '"';
    };

    std::string cmd = quote(exe_path);
    for(const auto& a : passthrough_args)
        cmd += ' ' + quote(a);
    cmd += " --part-b-row ";
    cmd += std::to_string(acc_raw);
    return cmd;
}

// ============================================================================
// Run one worker, read its stdout, parse results.
// Blocks until the worker exits.
// ============================================================================
static WorkerResult run_worker(
    const std::string& exe_path,
    const std::vector<std::string>& passthrough_args,
    int acc_raw)
{
    WorkerResult res;
    res.acc_raw = acc_raw;

    std::string cmdline = build_worker_cmdline(exe_path, passthrough_args, acc_raw);

    // Create anonymous pipe for stdout
    HANDLE pipe_rd = INVALID_HANDLE_VALUE;
    HANDLE pipe_wr = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength              = sizeof(sa);
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    if(!CreatePipe(&pipe_rd, &pipe_wr, &sa, 0)){
        res.error_detail = "CreatePipe failed";
        return res;
    }
    // Make read end non-inheritable
    SetHandleInformation(pipe_rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput  = pipe_wr;
    si.hStdError   = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<char> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back('\0');

    BOOL created = CreateProcessA(
        nullptr, cmd_buf.data(),
        nullptr, nullptr,
        TRUE,            // inherit handles (pipe_wr)
        0, nullptr, nullptr,
        &si, &pi);

    // Close write end in parent — worker holds it; when worker exits, read will EOF
    CloseHandle(pipe_wr);

    if(!created){
        res.error_detail = "CreateProcess failed for acc_raw=" + std::to_string(acc_raw);
        CloseHandle(pipe_rd);
        return res;
    }

    // Read all worker stdout
    std::string output;
    {
        char buf[4096];
        DWORD n = 0;
        while(ReadFile(pipe_rd, buf, sizeof(buf)-1, &n, nullptr) && n > 0){
            buf[n] = '\0';
            output += buf;
        }
    }
    CloseHandle(pipe_rd);

    // Wait for worker to exit and get exit code
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Parse output line by line
    std::istringstream iss(output);
    std::string line;
    while(std::getline(iss, line)){
        // Strip trailing \r
        if(!line.empty() && line.back() == '\r') line.pop_back();

        if(line.rfind("PARTB_ROW ", 0) == 0){
            // PARTB_ROW acc=N eva=N crystal=N enginemon=N
            WorkerResult::RowEntry e;
            if(sscanf(line.c_str(), "PARTB_ROW acc=%d eva=%d crystal=%d enginemon=%d",
                      &e.acc_raw, &e.eva_raw, &e.crystal, &e.enginemon) == 4){
                res.entries.push_back(e);
            }
        } else if(line.rfind("PARTB_DONE ", 0) == 0){
            // PARTB_DONE acc_min=N acc_max=N comparisons=N harness=N diffs=N
            int amin=0, amax=0, comp=0, harness=0, diffs=0;
            if(sscanf(line.c_str(),
                      "PARTB_DONE acc_min=%d acc_max=%d comparisons=%d harness=%d diffs=%d",
                      &amin, &amax, &comp, &harness, &diffs) == 5){
                res.comparisons = comp;
                res.harness     = harness;
                res.diffs       = diffs;
                res.done        = true;
            }
        }
    }

    // Validate
    // exit_code 0 = all match, 1 = diffs found — both are valid
    // exit_code 2 = HARNESS_ERROR, exit_code 3 = invalid args — both are failures
    const bool exit_ok = (exit_code == 0 || exit_code == 1);
    const int  expected_entries = 13; // EVA 1..13

    if(!exit_ok){
        res.error_detail = "worker exit=" + std::to_string((int)exit_code)
                         + " acc_raw=" + std::to_string(acc_raw);
    } else if(!res.done){
        res.error_detail = "worker missing PARTB_DONE acc_raw=" + std::to_string(acc_raw);
    } else if(res.harness > 0){
        res.error_detail = "worker harness_errors=" + std::to_string(res.harness)
                         + " acc_raw=" + std::to_string(acc_raw);
    } else if((int)res.entries.size() != expected_entries){
        res.error_detail = "worker got " + std::to_string(res.entries.size())
                         + " entries (expected " + std::to_string(expected_entries)
                         + ") acc_raw=" + std::to_string(acc_raw);
    } else if(res.comparisons != 3328){
        res.error_detail = "worker comparisons=" + std::to_string(res.comparisons)
                         + " expected=3328 acc_raw=" + std::to_string(acc_raw);
    } else {
        res.ok = true;
    }

    return res;
}

// ============================================================================
// Coordinator: run all 13 ACC rows with --jobs N parallelism.
// Returns EXIT_ALL_MATCH (0), EXIT_MISMATCH (1), or EXIT_HARNESS_ERROR (2).
// ============================================================================
static int run_part_b_coordinator(
    const std::string& exe_path,
    const std::vector<std::string>& passthrough_args,
    int jobs,
    bool verbose)
{
    constexpr int ACC_MIN = 1;
    constexpr int ACC_MAX = 13;
    constexpr int N_ROWS  = ACC_MAX - ACC_MIN + 1; // 13

    // Secondary guard: coordinator must never be entered with jobs > 2.
    // The CLI already rejects it; this catches any programmatic misuse.
    if(jobs < 1 || jobs > 2){
        std::cerr << "INTERNAL: run_part_b_coordinator called with jobs=" << jobs
                  << "; accuracy sweep supports --jobs 1 or 2 only\n";
        return crystal::oracle::EXIT_HARNESS_ERROR;
    }

    std::cout << "\n=== PART B: Screech ACC/EVA stage matrix ===\n"
              << "  ACC rows: " << ACC_MIN << ".." << ACC_MAX
              << "  jobs=" << jobs << "\n" << std::flush;

    std::vector<WorkerResult> results(N_ROWS);
    for(int i = 0; i < N_ROWS; ++i) results[i].acc_raw = ACC_MIN + i;

    // Process rows in batches of `jobs`
    int next_row = ACC_MIN;
    while(next_row <= ACC_MAX){
        int batch_end = std::min(next_row + jobs - 1, ACC_MAX);
        int batch_size = batch_end - next_row + 1;

        // Spawn batch
        struct Job {
            int acc_raw;
            HANDLE process;
            HANDLE pipe_rd;
            std::string output;
        };
        std::vector<Job> batch(batch_size);

        for(int b = 0; b < batch_size; ++b){
            int acc = next_row + b;
            batch[b].acc_raw = acc;
            batch[b].process = INVALID_HANDLE_VALUE;
            batch[b].pipe_rd = INVALID_HANDLE_VALUE;

            std::string cmdline = build_worker_cmdline(exe_path, passthrough_args, acc);

            HANDLE pipe_wr = INVALID_HANDLE_VALUE;
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
            if(!CreatePipe(&batch[b].pipe_rd, &pipe_wr, &sa, 0)){
                std::cerr << "CreatePipe failed acc=" << acc << "\n";
                // Mark as failed — collect rest of batch results then fail
                continue;
            }
            SetHandleInformation(batch[b].pipe_rd, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOA si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
            si.hStdOutput = pipe_wr;
            si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

            PROCESS_INFORMATION pi{};
            std::vector<char> cmd_buf(cmdline.begin(), cmdline.end());
            cmd_buf.push_back('\0');

            BOOL created = CreateProcessA(nullptr, cmd_buf.data(),
                                          nullptr, nullptr, TRUE, 0,
                                          nullptr, nullptr, &si, &pi);
            CloseHandle(pipe_wr);

            if(created){
                batch[b].process = pi.hProcess;
                CloseHandle(pi.hThread);
            } else {
                std::cerr << "CreateProcess failed acc=" << acc << "\n";
                CloseHandle(batch[b].pipe_rd);
                batch[b].pipe_rd = INVALID_HANDLE_VALUE;
            }
        }

        // Drain stdout from all batch workers with interleaved wait+read to avoid
        // pipe buffer deadlock. Windows pipe buffers are ~64KB; workers can emit
        // more (13 EVA × PARTB_ROW lines × worker startup output). Without reading
        // while waiting, a full pipe causes the worker to block on WriteFile, and
        // the coordinator to block on WaitForMultipleObjects — classic deadlock.
        //
        // Strategy: poll worker completion (1ms timeout) + drain any available data
        // from all pipes, repeat until all workers are done and pipes are empty.
        {
            std::vector<bool> proc_done(batch_size, false);
            int remaining_procs = 0;
            for(int b = 0; b < batch_size; ++b)
                if(batch[b].process != INVALID_HANDLE_VALUE) ++remaining_procs;

            while(remaining_procs > 0){
                // Poll each process; drain its pipe whether or not it's done
                for(int b = 0; b < batch_size; ++b){
                    // Drain available pipe data first (prevents buffer fill)
                    if(batch[b].pipe_rd != INVALID_HANDLE_VALUE){
                        DWORD avail = 0;
                        while(PeekNamedPipe(batch[b].pipe_rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0){
                            char buf[4096]; DWORD n = 0;
                            DWORD to_read = (avail < sizeof(buf)-1) ? avail : (DWORD)(sizeof(buf)-1);
                            if(ReadFile(batch[b].pipe_rd, buf, to_read, &n, nullptr) && n > 0){
                                buf[n] = '\0'; batch[b].output += buf;
                            } else break;
                        }
                    }

                    // Check if process is done
                    if(!proc_done[b] && batch[b].process != INVALID_HANDLE_VALUE){
                        DWORD st = WaitForSingleObject(batch[b].process, 0);
                        if(st == WAIT_OBJECT_0){
                            proc_done[b] = true;
                            --remaining_procs;
                            // Final drain after process exit
                            if(batch[b].pipe_rd != INVALID_HANDLE_VALUE){
                                char buf[4096]; DWORD n = 0;
                                while(ReadFile(batch[b].pipe_rd, buf, sizeof(buf)-1, &n, nullptr) && n > 0){
                                    buf[n] = '\0'; batch[b].output += buf;
                                }
                                CloseHandle(batch[b].pipe_rd);
                                batch[b].pipe_rd = INVALID_HANDLE_VALUE;
                            }
                        }
                    }
                }
                // Brief sleep to avoid burning CPU while polling
                if(remaining_procs > 0) Sleep(5);
            }
        }

        // Wait for all and collect exit codes + parse
        for(auto& job : batch){
            int idx = job.acc_raw - ACC_MIN;
            WorkerResult& res = results[idx];

            if(job.process == INVALID_HANDLE_VALUE){
                res.error_detail = "worker launch failed acc_raw=" + std::to_string(job.acc_raw);
                continue;
            }

            WaitForSingleObject(job.process, INFINITE);
            DWORD exit_code = 0;
            GetExitCodeProcess(job.process, &exit_code);
            CloseHandle(job.process);
            job.process = INVALID_HANDLE_VALUE;

            // Parse output
            std::istringstream iss(job.output);
            std::string line;
            while(std::getline(iss, line)){
                if(!line.empty() && line.back() == '\r') line.pop_back();
                if(line.rfind("PARTB_ROW ", 0) == 0){
                    WorkerResult::RowEntry e;
                    if(sscanf(line.c_str(), "PARTB_ROW acc=%d eva=%d crystal=%d enginemon=%d",
                              &e.acc_raw, &e.eva_raw, &e.crystal, &e.enginemon) == 4){
                        res.entries.push_back(e);
                    }
                } else if(line.rfind("PARTB_DONE ", 0) == 0){
                    int amin=0, amax=0, comp=0, harness=0, diffs=0;
                    if(sscanf(line.c_str(),
                              "PARTB_DONE acc_min=%d acc_max=%d comparisons=%d harness=%d diffs=%d",
                              &amin, &amax, &comp, &harness, &diffs) == 5){
                        res.comparisons = comp;
                        res.harness     = harness;
                        res.diffs       = diffs;
                        res.done        = true;
                    }
                }
            }

            bool exit_ok = (exit_code == 0 || exit_code == 1);
            if(!exit_ok){
                res.error_detail = "worker exit=" + std::to_string((int)exit_code)
                                 + " acc_raw=" + std::to_string(job.acc_raw);
            } else if(!res.done){
                res.error_detail = "worker missing PARTB_DONE acc_raw=" + std::to_string(job.acc_raw);
            } else if(res.harness > 0){
                res.error_detail = "worker harness_errors=" + std::to_string(res.harness)
                                 + " acc_raw=" + std::to_string(job.acc_raw);
            } else if((int)res.entries.size() != 13){
                res.error_detail = "worker got " + std::to_string(res.entries.size())
                                 + " entries (expected 13) acc_raw=" + std::to_string(job.acc_raw);
            } else if(res.comparisons != 3328){
                res.error_detail = "worker comparisons=" + std::to_string(res.comparisons)
                                 + " expected=3328 acc_raw=" + std::to_string(job.acc_raw);
            } else {
                res.ok = true;
            }
        }

        next_row = batch_end + 1;
    }

    // =========================================================================
    // Collect aggregate — sorted by ACC row (already in order by index)
    // =========================================================================
    bool any_failed    = false;
    int total_combos   = 0;
    int total_comps    = 0;
    int total_harness  = 0;
    int total_diffs    = 0;

    struct AggDiff {
        int acc_raw, eva_raw, crystal, enginemon;
    };
    std::vector<AggDiff> all_diffs;

    for(int i = 0; i < N_ROWS; ++i){
        const auto& res = results[i];
        if(!res.ok){
            std::cerr << "WORKER_FAIL: " << res.error_detail << "\n";
            any_failed = true;
            continue;
        }
        total_combos  += (int)res.entries.size();
        total_comps   += res.comparisons;
        total_harness += res.harness;
        total_diffs   += res.diffs;
        for(const auto& e : res.entries){
            if(e.crystal != e.enginemon)
                all_diffs.push_back({e.acc_raw, e.eva_raw, e.crystal, e.enginemon});
        }
    }

    if(any_failed){
        std::cerr << "PART B FAILED: one or more workers failed.\n";
        return crystal::oracle::EXIT_HARNESS_ERROR;
    }

    // Sort diffs by (acc_raw, eva_raw) for deterministic output
    std::sort(all_diffs.begin(), all_diffs.end(), [](const AggDiff& a, const AggDiff& b){
        if(a.acc_raw != b.acc_raw) return a.acc_raw < b.acc_raw;
        return a.eva_raw < b.eva_raw;
    });

    std::cout << "\n--- Part B Summary ---\n";
    std::cout << "  ACC rows tested:     1..13 (13 values)\n";
    std::cout << "  EVA range:           1..13 (13 values)\n";
    std::cout << "  Total combinations:  " << total_combos  << "\n";
    std::cout << "  Total RNG comparisons: " << total_comps << "\n";
    std::cout << "  Formula differences: " << total_diffs   << " stage combinations differ\n";
    std::cout << "  Harness errors:      " << total_harness << "\n";

    if(!all_diffs.empty()){
        std::cout << "\nFORMULA DIFFERENCES:\n";
        int show_max = verbose ? (int)all_diffs.size() : std::min(20,(int)all_diffs.size());
        for(int i = 0; i < show_max; ++i){
            const auto& d = all_diffs[i];
            std::cout << "  acc_stage=" << std::showpos << (d.acc_raw-7)
                      << " eva_stage=" << (d.eva_raw-7) << std::noshowpos
                      << " Crystal_max_hit_byte=0x"
                      << std::hex<<std::setw(2)<<std::setfill('0')<<(d.crystal<0?0:d.crystal)
                      << " Enginemon_max_hit_byte=0x"
                      << std::hex<<std::setw(2)<<std::setfill('0')<<(d.enginemon<0?0:d.enginemon)
                      << "\n" << std::dec;
        }
        if(!verbose && (int)all_diffs.size() > show_max)
            std::cout << "  ... (" << (all_diffs.size()-show_max) << " more; use --verbose)\n";
    }

    std::cout << "\n=== Part B Expected Totals ===\n";
    std::cout << "  combinations: " << total_combos << " (expected 169)\n";
    std::cout << "  comparisons:  " << total_comps  << " (expected 43264)\n";

    if(total_combos != 169 || total_comps != 43264){
        std::cerr << "PART B: aggregate totals mismatch.\n";
        return crystal::oracle::EXIT_HARNESS_ERROR;
    }

    return (total_diffs > 0) ? crystal::oracle::EXIT_MISMATCH
                              : crystal::oracle::EXIT_ALL_MATCH;
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char* argv[])
{
    crystal::oracle::RunnerConfig cfg;
    cfg.accuracy_sweep_a = false;
    cfg.accuracy_sweep_b = false;
    cfg.part_b_worker    = false;

    bool run_part_b_coord = false;
    int  coord_jobs       = 1;
    bool verbose          = false;

    // exe path (argv[0]) is passed through to workers
    std::string exe_path  = argc > 0 ? argv[0] : "oracle_accuracy_sweep.exe";

    // Args that are forwarded to runner_main (rom, sym, --verbose pass through)
    // and also to workers (rom, sym are positional and forwarded unchanged).
    // We collect passthrough_args separately for worker invocations.
    std::vector<std::string> passthrough_args; // rom + sym only

    std::vector<char*> runner_argv;
    runner_argv.push_back(argv[0]);

    for(int i = 1; i < argc; ++i){
        std::string a(argv[i]);

        if(a == "--part-a" || a == "--accuracy-sweep-a"){
            cfg.accuracy_sweep_a = true;

        } else if(a == "--part-b" || a == "--accuracy-sweep-b"){
            // Coordinator mode — do not forward to runner_main
            run_part_b_coord = true;

        } else if(a == "--all"){
            cfg.accuracy_sweep_a = true;
            run_part_b_coord     = true;

        } else if(a == "--part-b-row" && i + 1 < argc){
            // Worker mode: single ACC row
            int v = std::atoi(argv[++i]);
            cfg.part_b_worker    = true;
            cfg.accuracy_sweep_b = false; // worker uses part_b_worker flag
            cfg.sweep_acc_min    = v;
            cfg.sweep_acc_max    = v;
            // Forward --part-b-row to runner_main so it sees accuracy_sweep_b path
            // (runner_main checks accuracy_sweep_b || part_b_worker in the dispatch)

        } else if(a == "--jobs" && i + 1 < argc){
            int v = std::atoi(argv[++i]);
            // Hard cap: accuracy sweep supports --jobs 1 or 2 only.
            // Values outside this range are rejected before any worker is spawned.
            if(v < 1 || v > 2){
                fprintf(stderr,
                    "oracle_accuracy_sweep: accuracy sweep supports --jobs 1 or 2 only"
                    " (got %d)\n", v);
                return crystal::oracle::EXIT_INVALID_ARGS;
            }
            coord_jobs = v;
            // Do NOT forward --jobs to runner_main (it affects the move-oracle parallelism)

        } else if(a == "--verbose" || a == "-v"){
            verbose = true;
            cfg.verbose = true;
            runner_argv.push_back(argv[i]);

        } else {
            // Positional args (rom, sym) and unknown flags go to runner_main
            runner_argv.push_back(argv[i]);
            // Collect the first two non-flag positional args as passthrough_args for workers
            if(a[0] != '-' && passthrough_args.size() < 2)
                passthrough_args.push_back(a);
        }
    }
    runner_argv.push_back(nullptr);

    // =========================================================================
    // Worker mode: delegate entirely to runner_main
    // =========================================================================
    if(cfg.part_b_worker){
        return crystal::oracle::runner_main(
            (int)runner_argv.size() - 1, runner_argv.data(), cfg);
    }

    // =========================================================================
    // Part A: delegate to runner_main as before
    // =========================================================================
    if(cfg.accuracy_sweep_a){
        int ret = crystal::oracle::runner_main(
            (int)runner_argv.size() - 1, runner_argv.data(), cfg);
        if(!run_part_b_coord) return ret;
        // If --all, continue to Part B coordinator below
        // (Part A already printed its own output)
        // Reset accuracy_sweep_a so runner_main doesn't re-run it in worker sub-calls
        cfg.accuracy_sweep_a = false;
        if(ret == crystal::oracle::EXIT_HARNESS_ERROR) return ret;
    }

    // =========================================================================
    // Part B coordinator
    // =========================================================================
    if(run_part_b_coord){
        if(passthrough_args.size() < 2){
            fprintf(stderr,
                "oracle_accuracy_sweep: need <rom> <sym> positional args\n"
                "Usage: oracle_accuracy_sweep <rom> <sym> --part-b [--jobs N]\n");
            return crystal::oracle::EXIT_INVALID_ARGS;
        }
        return run_part_b_coordinator(exe_path, passthrough_args, coord_jobs, verbose);
    }

    // No mode selected
    if(!cfg.accuracy_sweep_a && !run_part_b_coord && !cfg.part_b_worker){
        fprintf(stderr,
            "oracle_accuracy_sweep <rom> <sym> --part-a|--part-b|--all [--verbose] [--jobs 1|2]\n"
            "  --jobs 1|2      Part B: worker concurrency (1 or 2 only; default 1)\n"
            "  --part-b-row N  (worker mode, invoked by coordinator)\n");
        return crystal::oracle::EXIT_INVALID_ARGS;
    }

    return crystal::oracle::EXIT_ALL_MATCH;
}
