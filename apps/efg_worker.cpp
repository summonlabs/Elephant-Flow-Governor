// Elephant Flow Governor - cluster worker process.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A worker is a publisher, never an authority. It registers one incarnation,
// submits generation bound evidence for its shard, and leaves. --abort-after
// terminates the process abruptly, without a bye and without flushing, which is
// how the multiprocess tests exercise publisher death.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "cli.hpp"

int main(int argc, char** argv) {
    using namespace efg;
    using namespace efg::cli;

    const Args args(argc, argv);
    if (args.has("help")) {
        std::fputs(Args::usage().c_str(), stdout);
        return 0;
    }

    const ScenarioSpec spec = scenario_from_args(args);
    const Status spec_status = spec.validate();
    if (!spec_status.ok()) {
        std::fprintf(stderr, "scenario is invalid: %s\n", spec_status.to_string().c_str());
        return 2;
    }

    BootIdentity boot;
    boot.boot = BootId{args.get_u64("boot", 2)};
    boot.epoch = EpochId{args.get_u64("epoch", 1)};
    boot.started_at = 1000;
    boot.monotonic_seed = spec.population.seed;

    WorkerConfig config;
    config.port = static_cast<u16>(args.get_u64("port", 0));
    config.boot = boot;
    config.requested_worker = WorkerId{args.get_u64("worker-id", 0)};
    config.session_nonce = args.get_u64("nonce", 1);
    config.label = args.get("label", "worker");
    config.start_tick = 1000;
    config.limits.max_batch = args.get_size("max-batch", 64);

    const u64 shard = args.get_u64("shard", 0);
    const u64 shards = args.get_u64("shards", 1);
    const u64 publisher = args.get_u64("publisher", 2);

    StatusOr<SampleStream> stream = build_stream(spec, boot, PublisherId{publisher}, shard, shards);
    if (!stream.ok()) {
        std::fprintf(stderr, "stream could not be built: %s\n", stream.status().to_string().c_str());
        return 2;
    }

    StatusOr<std::unique_ptr<Worker>> worker = Worker::connect(config);
    if (!worker.ok()) {
        std::fprintf(stderr, "connect failed: %s\n", worker.status().to_string().c_str());
        return 3;
    }
    const Status registration = worker.value()->register_session();
    if (!registration.ok()) {
        std::printf("registration_fenced=%s detail=%s\n",
                    std::string{to_string(worker.value()->last_fence())}.c_str(),
                    worker.value()->last_fence_detail().c_str());
        std::fflush(stdout);
        return 4;
    }
    std::printf("registered_worker=%llu\n",
                static_cast<unsigned long long>(worker.value()->worker_id().value()));
    std::fflush(stdout);

    // Readiness and progress files exist so that a supervising process can wait
    // on definite events instead of guessing how long a peer needs.
    const std::string ready_file = args.get("ready-file");
    if (!ready_file.empty()) {
        const Status written = write_text_file(ready_file, "ready\n");
        if (!written.ok()) {
            return 3;
        }
    }
    const std::string progress_file = args.get("progress-file");

    const u64 abort_after = args.get_u64("abort-after", 0);
    u64 batches = 0;
    u64 pending = 0;
    for (const FlowSample& sample : stream.value().samples) {
        const Status submitted = worker.value()->submit_evidence(sample);
        if (!submitted.ok()) {
            std::fprintf(stderr, "submit failed: %s\n", submitted.to_string().c_str());
            return 5;
        }
        ++pending;
        if (pending >= config.limits.max_batch) {
            const Status flushed = worker.value()->flush();
            if (!flushed.ok()) {
                std::printf("batch_fenced=%s detail=%s\n",
                            std::string{to_string(worker.value()->last_fence())}.c_str(),
                            worker.value()->last_fence_detail().c_str());
                std::fflush(stdout);
                return 6;
            }
            pending = 0;
            ++batches;
            if (!progress_file.empty()) {
                std::string marker;
                append_u64(marker, batches);
                marker.push_back('\n');
                (void)write_text_file(progress_file, marker);
            }
            if (abort_after != 0 && batches >= abort_after) {
                std::printf("aborting_after_batches=%llu\n",
                            static_cast<unsigned long long>(batches));
                std::fflush(stdout);
                // Hard, unannounced process death: no destructors, no flush, no bye.
                std::_Exit(9);
            }
        }
    }

    const Status flushed = worker.value()->flush();
    if (!flushed.ok()) {
        std::printf("final_flush_fenced=%s detail=%s\n",
                    std::string{to_string(worker.value()->last_fence())}.c_str(),
                    worker.value()->last_fence_detail().c_str());
        std::fflush(stdout);
        return 6;
    }
    const Status bye = worker.value()->bye("shard complete");
    if (!bye.ok()) {
        std::fprintf(stderr, "bye failed: %s\n", bye.to_string().c_str());
        return 6;
    }

    std::string out{"worker="};
    append_u64(out, worker.value()->worker_id().value());
    out.append(" frames=");
    append_u64(out, worker.value()->frames_sent());
    out.append(" samples=");
    append_u64(out, worker.value()->samples_sent());
    out.append(" accepted=");
    append_u64(out, worker.value()->accepted());
    out.append(" duplicates=");
    append_u64(out, worker.value()->duplicates());
    out.append(" rejected=");
    append_u64(out, worker.value()->rejected());
    out.push_back('\n');
    std::fputs(out.c_str(), stdout);

    const std::string report_path = args.get("report");
    if (!report_path.empty()) {
        return write_text_file(report_path, out).ok() ? 0 : 7;
    }
    return 0;
}
