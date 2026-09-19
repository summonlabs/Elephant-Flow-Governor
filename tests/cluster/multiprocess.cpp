// Elephant Flow Governor - real multiprocess cluster tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// These tests spawn real operating system processes that talk over real loopback
// sockets with the real framing layer. Nothing is simulated in process. A worker
// that is killed mid stream is killed with TerminateProcess, exactly as an
// operator would find it.
//
// No test timeout is used anywhere. Where the test must wait for a peer to make
// progress it waits on a definite event (a process handle, a published port)
// with a bounded number of attempts, and fails loudly if the event never occurs.

#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "efg/efg.hpp"
#include "fixtures.hpp"
#include "test_harness.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace efg;
using namespace efgtest;

namespace {

#ifdef _WIN32

struct ChildProcess {
    PROCESS_INFORMATION info{};
    bool started{false};
};

ChildProcess spawn(const std::string& command_line) {
    ChildProcess child;
    std::string mutable_line = command_line;
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    // Child output is routed to NUL so that a passing suite stays readable, and
    // a child never blocks writing into a pipe nobody reads.
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE sink = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &attributes, OPEN_EXISTING, 0, nullptr);
    startup.hStdOutput = sink;
    startup.hStdError = sink;
    startup.hStdInput = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &attributes, OPEN_EXISTING, 0, nullptr);
    const BOOL ok = CreateProcessA(nullptr, mutable_line.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child.info);
    if (sink != INVALID_HANDLE_VALUE) {
        CloseHandle(sink);
    }
    if (startup.hStdInput != INVALID_HANDLE_VALUE) {
        CloseHandle(startup.hStdInput);
    }
    child.started = ok != FALSE;
    return child;
}

int wait_for(ChildProcess& child) {
    if (!child.started) {
        return -1;
    }
    WaitForSingleObject(child.info.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(child.info.hProcess, &code);
    CloseHandle(child.info.hThread);
    CloseHandle(child.info.hProcess);
    child.started = false;
    return static_cast<int>(code);
}

bool terminate(ChildProcess& child) {
    if (!child.started) {
        return false;
    }
    const bool killed = TerminateProcess(child.info.hProcess, 0xBEEF) != FALSE;
    WaitForSingleObject(child.info.hProcess, INFINITE);
    CloseHandle(child.info.hThread);
    CloseHandle(child.info.hProcess);
    child.started = false;
    return killed;
}

/// Owns the coordinator's serving thread. On destruction the coordinator is
/// asked to stop and the thread is joined, so a failed assertion unwinds
/// cleanly instead of leaving a joinable thread behind.
class ServerGuard {
public:
    ServerGuard(Coordinator& coordinator, std::thread thread)
        : coordinator_(&coordinator), thread_(std::move(thread)) {}

    ~ServerGuard() {
        if (thread_.joinable()) {
            (void)coordinator_->stop();
            thread_.join();
        }
    }

    ServerGuard(const ServerGuard&) = delete;
    ServerGuard& operator=(const ServerGuard&) = delete;

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    Coordinator* coordinator_;
    std::thread thread_;
};

std::string quote(const std::string& text) {
    std::string out{"\""};
    out.append(text);
    out.push_back('"');
    return out;
}

bool wait_for_file(const std::filesystem::path& path, int attempts) {
    for (int i = 0; i < attempts; ++i) {
        std::error_code error;
        if (std::filesystem::exists(path, error) && !error) {
            return true;
        }
        Sleep(25);
    }
    return false;
}

std::string read_all(const std::filesystem::path& path) {
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return {};
    }
    std::string out;
    char buffer[512];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out.append(buffer, read);
    }
    std::fclose(file);
    return out;
}

class ScratchDirectory {
public:
    explicit ScratchDirectory(const std::string& name) {
        path_ = std::filesystem::temp_directory_path() /
                (name + "-" + unique_scratch_suffix());
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
    }
    ~ScratchDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    [[nodiscard]] std::filesystem::path file(const std::string& name) const {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

std::string scenario_arguments() {
    return std::string{" --mice 12 --elephants 4 --ticks 16 --window 4 --resources 2"
                       " --capacity 65536 --reserved 8192 --paths 2 --path-resources 2"};
}

PopulationSpec cluster_population() {
    PopulationSpec spec;
    spec.mice = 12;
    spec.elephants = 4;
    spec.ticks = 16;
    spec.window_ticks = 4;
    spec.resources = 2;
    spec.resource_capacity = 65536;
    spec.resource_reserved = 8192;
    spec.paths = 2;
    spec.path_resources = 2;
    return spec;
}

constexpr u64 kClusterPolicyValidity = 16 * 8 + 64;

/// Capacity snapshots matching the resources the worker generated evidence for.
std::vector<ResourceCapacity> cluster_capacities(const PopulationSpec& spec,
                                                 const BootIdentity& who) {
    std::vector<ResourceCapacity> out;
    StatusOr<ValidityWindow> validity =
        make_validity(1000, kClusterPolicyValidity + spec.ticks * 4u + 64u);
    for (std::size_t r = 0; r < spec.resources; ++r) {
        ResourceCapacity value;
        value.resource = ResourceId{r + 1};
        value.generation = Generation::initial();
        value.snapshot = CapacitySnapshotId{r + 1};
        value.span = TickSpan{1000, 1000 + spec.ticks + 1};
        value.capacity = spec.resource_capacity * kKiloTickScale;
        value.reserved = spec.resource_reserved * kKiloTickScale;
        if (validity.ok()) {
            value.validity = validity.value();
        }
        value.provenance = provenance(1, who.boot.value(), who.epoch.value(), r + 1, 1000);
        out.push_back(value);
    }
    return out;
}

/// Path descriptors matching the identifiers the worker shards reference.
std::vector<PathDescriptor> cluster_paths(const PopulationSpec& spec, const BootIdentity& who) {
    std::vector<PathDescriptor> out;
    StatusOr<ValidityWindow> validity =
        make_validity(1000, kClusterPolicyValidity + spec.ticks * 4u + 64u);
    for (std::size_t index = 0; index < spec.paths; ++index) {
        PathDescriptor value;
        value.path = PathId{index + 1};
        value.generation = Generation::initial();
        for (std::size_t k = 0; k < spec.path_resources; ++k) {
            value.resources.push_back(ResourceId{(index + k) % spec.resources + 1});
        }
        if (validity.ok()) {
            value.validity = validity.value();
        }
        value.provenance = provenance(1, who.boot.value(), who.epoch.value(), index + 2, 1000);
        out.push_back(value);
    }
    return out;
}

CoordinatorConfig cluster_config(std::size_t sessions, BootIdentity identity) {
    const PopulationSpec spec = cluster_population();
    CoordinatorConfig config;
    config.boot = identity;
    config.start_tick = 1000;
    config.expected_sessions = sessions;
    config.limits.max_batch = 16;
    StatusOr<PolicyDocument> policy = make_benchmark_policy(
        spec, PolicyId{7}, Generation::initial(), 1000, kClusterPolicyValidity);
    if (policy.ok()) {
        config.policy = policy.value();
    }
    BootIdentity publisher;
    publisher.boot = BootId{99};
    publisher.epoch = identity.epoch;
    publisher.started_at = 1000;
    config.capacities = cluster_capacities(spec, publisher);
    config.paths = cluster_paths(spec, publisher);
    return config;
}
#endif  // _WIN32

}  // namespace

EFG_TEST(cluster, two_worker_processes_complete_over_real_sockets) {
#ifdef _WIN32
    EFG_CHECK(std::string{EFG_TEST_WORKER_BINARY}.size() > 0);
    ScratchDirectory scratch("efg-cluster-two");

    CoordinatorConfig config = cluster_config(2, boot(1, 1));
    StatusOr<std::unique_ptr<Coordinator>> coordinator = Coordinator::create(config);
    EFG_REQUIRE(coordinator.ok());
    EFG_CHECK_STATUS_OK(coordinator.value()->listen());
    const u16 port = coordinator.value()->port();
    EFG_CHECK(port != 0);

    ServerGuard server(*coordinator.value(),
                         std::thread([&]() { (void)coordinator.value()->run(); }));

    ChildProcess first = spawn(quote(std::string{EFG_TEST_WORKER_BINARY}) + scenario_arguments() +
                               " --port " + std::to_string(port) +
                               " --boot 7 --epoch 1 --nonce 11 --worker-id 1 --label w1"
                               " --max-batch 6 --shard 0 --shards 2");
    EFG_REQUIRE(first.started);
    ChildProcess second = spawn(quote(std::string{EFG_TEST_WORKER_BINARY}) + scenario_arguments() +
                                " --port " + std::to_string(port) +
                                " --boot 8 --epoch 1 --nonce 12 --worker-id 2 --label w2"
                                " --max-batch 6 --shard 1 --shards 2");
    EFG_REQUIRE(second.started);
    EFG_CHECK_EQ(wait_for(first), 0);
    EFG_CHECK_EQ(wait_for(second), 0);
    server.join();

    const std::vector<SessionOutcome>& sessions = coordinator.value()->sessions();
    EFG_CHECK_EQ(sessions.size(), 2u);
    for (const SessionOutcome& session : sessions) {
        EFG_CHECK(!session.fenced);
        EFG_CHECK(session.clean_bye);
        EFG_CHECK(session.accepted > 0);
        EFG_CHECK_EQ(session.rejected, 0u);
    }
    EFG_CHECK(coordinator.value()->governor().flow_count() > 0);
    EFG_CHECK(coordinator.value()->governor().counters().elephant_decisions > 0);
    EFG_CHECK(coordinator.value()->governor().intents().size() > 0);

    const Status write = coordinator.value()->write_report(scratch.file("report.json"));
    EFG_CHECK_STATUS_OK(write);
    const std::string report = read_all(scratch.file("report.json"));
    EFG_CHECK(report.find("\"sessions\"") != std::string::npos);
    EFG_CHECK(report.find("\"fenced\": false") != std::string::npos);
#else
    EFG_FAIL("the multiprocess cluster suite is validated on Windows only");
#endif
}

EFG_TEST(cluster, an_abruptly_killed_worker_does_not_take_the_coordinator_down) {
#ifdef _WIN32
    ScratchDirectory scratch("efg-cluster-kill");
    CoordinatorConfig config = cluster_config(2, boot(1, 1));
    StatusOr<std::unique_ptr<Coordinator>> coordinator = Coordinator::create(config);
    EFG_REQUIRE(coordinator.ok());
    EFG_CHECK_STATUS_OK(coordinator.value()->listen());
    const u16 port = coordinator.value()->port();

    ServerGuard server(*coordinator.value(),
                         std::thread([&]() { (void)coordinator.value()->run(); }));

    // The first worker dies abruptly after its first flushed batch: no destructors,
    // no flush, no bye.
    ChildProcess doomed = spawn(quote(std::string{EFG_TEST_WORKER_BINARY}) + scenario_arguments() +
                                " --port " + std::to_string(port) +
                                " --boot 7 --epoch 1 --nonce 21 --worker-id 1 --label doomed"
                                " --max-batch 2 --abort-after 1 --shard 0 --shards 2");
    EFG_REQUIRE(doomed.started);
    const int doomed_code = wait_for(doomed);
    EFG_CHECK_EQ(doomed_code, 9);

    ChildProcess survivor = spawn(quote(std::string{EFG_TEST_WORKER_BINARY}) + scenario_arguments() +
                                  " --port " + std::to_string(port) +
                                  " --boot 8 --epoch 1 --nonce 22 --worker-id 2 --label survivor"
                                  " --max-batch 6 --shard 1 --shards 2");
    EFG_REQUIRE(survivor.started);
    EFG_CHECK_EQ(wait_for(survivor), 0);
    server.join();

    const std::vector<SessionOutcome>& sessions = coordinator.value()->sessions();
    EFG_CHECK_EQ(sessions.size(), 2u);
    EFG_CHECK(!sessions[0].clean_bye);
    EFG_CHECK(sessions[0].accepted > 0);
    EFG_CHECK(sessions[1].clean_bye);
    EFG_CHECK(sessions[0].worker.value() != sessions[1].worker.value());

    // The publisher died, so the evidence it supplied ages out and the flows it
    // was reporting on are suspended on the next sweep rather than remaining
    // authoritative elephants forever.
    const Tick sweep = 1000 + 400;
    StatusOr<DecisionBatch> batch = coordinator.value()->governor().tick(sweep);
    EFG_REQUIRE(batch.ok());
    EFG_CHECK(batch.value().suspensions > 0);
    EFG_CHECK_EQ(coordinator.value()->governor().intents().live_count(), 0u);
    EFG_CHECK(coordinator.value()->governor().counters().authority_revocations > 0);
#else
    EFG_FAIL("the multiprocess cluster suite is validated on Windows only");
#endif
}

EFG_TEST(cluster, a_terminated_worker_is_detected_by_the_coordinator) {
#ifdef _WIN32
    ScratchDirectory scratch("efg-cluster-terminate");
    const std::filesystem::path ready_file = scratch.file("ready.txt");
    const std::filesystem::path progress_file = scratch.file("progress.txt");

    CoordinatorConfig config = cluster_config(2, boot(1, 1));
    StatusOr<std::unique_ptr<Coordinator>> coordinator = Coordinator::create(config);
    EFG_REQUIRE(coordinator.ok());
    EFG_CHECK_STATUS_OK(coordinator.value()->listen());
    const u16 port = coordinator.value()->port();
    ServerGuard server(*coordinator.value(),
                         std::thread([&]() { (void)coordinator.value()->run(); }));

    // A worker that streams continuously without ever sending a bye.
    ChildProcess streaming = spawn(quote(std::string{EFG_TEST_WORKER_BINARY}) + scenario_arguments() +
                                   " --port " + std::to_string(port) +
                                   " --boot 7 --epoch 1 --nonce 31 --worker-id 1 --label streamer"
                                   " --max-batch 1 --mice 132 --elephants 32 --ticks 240"
                                   " --shard 0 --shards 1 --ready-file " + quote(ready_file.string()) +
                                   " --progress-file " + quote(progress_file.string()));
    EFG_REQUIRE(streaming.started);

    // Wait on definite events published by the worker: it is registered, and it
    // has had at least one batch accepted. No coordinator state is read while
    // run() owns it, so there is no race and no timing guess.
    EFG_REQUIRE(wait_for_file(ready_file, 800));
    EFG_REQUIRE(wait_for_file(progress_file, 800));
    // The worker must still be mid stream; if it had already finished there
    // would be nothing abrupt about its death.
    EFG_REQUIRE(WaitForSingleObject(streaming.info.hProcess, 0) == WAIT_TIMEOUT);
    EFG_CHECK(terminate(streaming));

    ChildProcess closer = spawn(quote(std::string{EFG_TEST_WORKER_BINARY}) + scenario_arguments() +
                                " --port " + std::to_string(port) +
                                " --boot 9 --epoch 1 --nonce 32 --worker-id 2 --label closer"
                                " --max-batch 4 --shard 1 --shards 2");
    EFG_REQUIRE(closer.started);
    EFG_CHECK_EQ(wait_for(closer), 0);
    server.join();

    EFG_CHECK_EQ(coordinator.value()->sessions().size(), 2u);
    EFG_CHECK(!coordinator.value()->sessions()[0].clean_bye);
    EFG_CHECK(coordinator.value()->sessions()[0].accepted > 0);
    EFG_CHECK(coordinator.value()->sessions()[1].clean_bye);
#else
    EFG_FAIL("the multiprocess cluster suite is validated on Windows only");
#endif
}

EFG_TEST(cluster, a_worker_from_an_older_epoch_is_fenced) {
#ifdef _WIN32
    CoordinatorConfig config = cluster_config(2, boot(1, 5));
    StatusOr<std::unique_ptr<Coordinator>> coordinator = Coordinator::create(config);
    EFG_REQUIRE(coordinator.ok());
    EFG_CHECK_STATUS_OK(coordinator.value()->listen());
    const u16 port = coordinator.value()->port();
    ServerGuard server(*coordinator.value(),
                         std::thread([&]() { (void)coordinator.value()->run(); }));

    ChildProcess stale = spawn(quote(std::string{EFG_TEST_WORKER_BINARY}) + scenario_arguments() +
                               " --port " + std::to_string(port) +
                               " --boot 7 --epoch 4 --nonce 41 --worker-id 1 --label stale"
                               " --max-batch 4 --shard 0 --shards 2");
    EFG_REQUIRE(stale.started);
    EFG_CHECK_EQ(wait_for(stale), 4);

    ChildProcess current = spawn(quote(std::string{EFG_TEST_WORKER_BINARY}) + scenario_arguments() +
                                 " --port " + std::to_string(port) +
                                 " --boot 8 --epoch 5 --nonce 42 --worker-id 2 --label current"
                                 " --max-batch 6 --shard 1 --shards 2");
    EFG_REQUIRE(current.started);
    EFG_CHECK_EQ(wait_for(current), 0);
    server.join();

    const std::vector<SessionOutcome>& sessions = coordinator.value()->sessions();
    EFG_REQUIRE(sessions.size() == 2u);
    EFG_CHECK(sessions[0].fenced);
    EFG_CHECK_EQ(sessions[0].fence, FenceReason::EpochStale);
    EFG_CHECK_EQ(sessions[0].accepted, 0u);
    EFG_CHECK(!sessions[1].fenced);
    EFG_CHECK(sessions[1].accepted > 0);
#else
    EFG_FAIL("the multiprocess cluster suite is validated on Windows only");
#endif
}

EFG_TEST(cluster, a_replayed_session_nonce_is_fenced) {
#ifdef _WIN32
    CoordinatorConfig config = cluster_config(2, boot(1, 1));
    StatusOr<std::unique_ptr<Coordinator>> coordinator = Coordinator::create(config);
    EFG_REQUIRE(coordinator.ok());
    EFG_CHECK_STATUS_OK(coordinator.value()->listen());
    const u16 port = coordinator.value()->port();
    ServerGuard server(*coordinator.value(),
                         std::thread([&]() { (void)coordinator.value()->run(); }));

    ChildProcess first = spawn(quote(std::string{EFG_TEST_WORKER_BINARY}) + scenario_arguments() +
                               " --port " + std::to_string(port) +
                               " --boot 7 --epoch 1 --nonce 51 --worker-id 1 --label first"
                               " --max-batch 6 --shard 0 --shards 2");
    EFG_REQUIRE(first.started);
    EFG_CHECK_EQ(wait_for(first), 0);

    // Same nonce, replayed by a different incarnation: fenced before any evidence
    // from it can be applied.
    ChildProcess replay = spawn(quote(std::string{EFG_TEST_WORKER_BINARY}) + scenario_arguments() +
                                " --port " + std::to_string(port) +
                                " --boot 8 --epoch 1 --nonce 51 --worker-id 1 --label replay"
                                " --max-batch 6 --shard 1 --shards 2");
    EFG_REQUIRE(replay.started);
    EFG_CHECK_EQ(wait_for(replay), 4);
    server.join();

    const std::vector<SessionOutcome>& sessions = coordinator.value()->sessions();
    EFG_REQUIRE(sessions.size() == 2u);
    EFG_CHECK(!sessions[0].fenced);
    EFG_CHECK(sessions[1].fenced);
    EFG_CHECK_EQ(sessions[1].fence, FenceReason::WorkerDuplicate);
    EFG_CHECK_EQ(sessions[1].accepted, 0u);
#else
    EFG_FAIL("the multiprocess cluster suite is validated on Windows only");
#endif
}

EFG_TEST(cluster, a_worker_that_never_sends_a_hello_is_not_accepted) {
#ifdef _WIN32
    CoordinatorConfig config = cluster_config(1, boot(1, 1));
    StatusOr<std::unique_ptr<Coordinator>> coordinator = Coordinator::create(config);
    EFG_REQUIRE(coordinator.ok());
    EFG_CHECK_STATUS_OK(coordinator.value()->listen());
    const u16 port = coordinator.value()->port();
    ServerGuard server(*coordinator.value(),
                         std::thread([&]() { (void)coordinator.value()->run(); }));

    // A raw socket that connects and closes without a hello.
    {
        StatusOr<TcpStream> stream = connect_loopback(port);
        EFG_REQUIRE(stream.ok());
        EFG_CHECK_STATUS_OK(stream.value().close());
    }
    server.join();

    const std::vector<SessionOutcome>& sessions = coordinator.value()->sessions();
    EFG_REQUIRE(sessions.size() == 1u);
    EFG_CHECK(sessions[0].fenced || !sessions[0].clean_bye);
    EFG_CHECK_EQ(sessions[0].accepted, 0u);
#else
    EFG_FAIL("the multiprocess cluster suite is validated on Windows only");
#endif
}

EFG_TEST(cluster, the_coordinator_binary_serves_worker_processes_end_to_end) {
#ifdef _WIN32
    ScratchDirectory scratch("efg-cluster-binary");
    const std::filesystem::path port_file = scratch.file("port.txt");
    const std::filesystem::path report_file = scratch.file("report.json");
    const std::filesystem::path done_file = scratch.file("done.txt");

    ChildProcess coordinator =
        spawn(quote(std::string{EFG_TEST_COORDINATOR_BINARY}) + scenario_arguments() +
              " --boot 1 --epoch 1 --workers 2 --final-tick 1400" + " --port-file " +
              quote(port_file.string()) + " --report " + quote(report_file.string()) +
              " --done-file " + quote(done_file.string()));
    EFG_REQUIRE(coordinator.started);
    EFG_REQUIRE(wait_for_file(port_file, 800));

    const std::string port_text = read_all(port_file);
    EFG_REQUIRE(!port_text.empty());
    u64 port = 0;
    EFG_REQUIRE(parse_u64(port_text.substr(0, port_text.find_first_of("\r\n")), port));
    EFG_CHECK(port > 0);

    ChildProcess first = spawn(quote(std::string{EFG_TEST_WORKER_BINARY}) + scenario_arguments() +
                               " --port " + std::to_string(port) +
                               " --boot 7 --epoch 1 --nonce 61 --worker-id 1 --label a"
                               " --max-batch 6 --shard 0 --shards 2");
    EFG_REQUIRE(first.started);
    ChildProcess second = spawn(quote(std::string{EFG_TEST_WORKER_BINARY}) + scenario_arguments() +
                                " --port " + std::to_string(port) +
                                " --boot 8 --epoch 1 --nonce 62 --worker-id 2 --label b"
                                " --max-batch 6 --shard 1 --shards 2");
    EFG_REQUIRE(second.started);

    EFG_CHECK_EQ(wait_for(second), 0);
    EFG_CHECK_EQ(wait_for(first), 0);
    EFG_CHECK_EQ(wait_for(coordinator), 0);
    EFG_CHECK(std::filesystem::exists(done_file));

    const std::string report = read_all(report_file);
    EFG_CHECK(!report.empty());
    EFG_CHECK(report.find("\"coordinator_epoch\": 1") != std::string::npos);
    EFG_CHECK(report.find("\"fenced\": false") != std::string::npos);
    EFG_CHECK(report.find("\"clean_bye\": true") != std::string::npos);
    EFG_CHECK(report.find("\"suspended_decisions\"") != std::string::npos);
#else
    EFG_FAIL("the multiprocess cluster suite is validated on Windows only");
#endif
}
