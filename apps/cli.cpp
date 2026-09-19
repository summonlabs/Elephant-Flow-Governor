// Elephant Flow Governor - shared command line support.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "cli.hpp"

#include <cstdio>
#include <cstdlib>

#include "efg/text.hpp"

namespace efg::cli {

Args::Args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string token{argv[i]};
        if (token.rfind("--", 0) != 0) {
            continue;
        }
        std::string name = token.substr(2);
        std::string value{"1"};
        const std::size_t equals = name.find('=');
        if (equals != std::string::npos) {
            value = name.substr(equals + 1);
            name = name.substr(0, equals);
        } else if (i + 1 < argc) {
            std::string next{argv[i + 1]};
            if (next.rfind("--", 0) != 0) {
                value = next;
                ++i;
            }
        }
        values_.emplace_back(std::move(name), std::move(value));
    }
}

bool Args::has(std::string_view name) const {
    for (const auto& entry : values_) {
        if (entry.first == name) {
            return true;
        }
    }
    return false;
}

std::string Args::get(std::string_view name, std::string fallback) const {
    // The last occurrence wins, so a later option overrides an earlier one. A
    // first-wins rule silently ignores the override an operator just typed.
    for (auto entry = values_.rbegin(); entry != values_.rend(); ++entry) {
        if (entry->first == name) {
            return entry->second;
        }
    }
    return fallback;
}

std::uint64_t Args::get_u64(std::string_view name, std::uint64_t fallback) const {
    const std::string raw = get(name);
    if (raw.empty()) {
        return fallback;
    }
    u64 value = 0;
    if (!parse_u64(raw, value)) {
        return fallback;
    }
    return value;
}

std::size_t Args::get_size(std::string_view name, std::size_t fallback) const {
    const u64 value = get_u64(name, static_cast<u64>(fallback));
    if (value > static_cast<u64>(limits::kMaxBenchmarkPopulation)) {
        return fallback;
    }
    return static_cast<std::size_t>(value);
}

bool Args::get_bool(std::string_view name, bool fallback) const {
    const std::string raw = get(name);
    if (raw.empty()) {
        return fallback;
    }
    if (raw == "1" || raw == "true" || raw == "yes") {
        return true;
    }
    if (raw == "0" || raw == "false" || raw == "no") {
        return false;
    }
    return fallback;
}

std::string Args::usage() {
    return std::string{
        "options: --mice N --elephants N --seed N --ticks N --window N --resources N\n"
        "         --capacity N --reserved N --paths N --path-resources N --complexity N\n"
        "         --protected-bp N --abandon-bp N\n"};
}

PopulationSpec population_from_args(const Args& args) {
    PopulationSpec spec;
    spec.mice = args.get_size("mice", 64);
    spec.elephants = args.get_size("elephants", 16);
    spec.seed = args.get_u64("seed", 0x5EED1234ull);
    spec.ticks = args.get_u64("ticks", 64);
    spec.window_ticks = args.get_u64("window", 4);
    spec.mouse_rate_lo = args.get_u64("mouse-rate-lo", 1);
    spec.mouse_rate_hi = args.get_u64("mouse-rate-hi", 64);
    spec.elephant_rate_lo = args.get_u64("elephant-rate-lo", 4096);
    spec.elephant_rate_hi = args.get_u64("elephant-rate-hi", 16384);
    spec.resources = args.get_size("resources", 4);
    spec.resource_capacity = args.get_u64("capacity", 65536);
    spec.resource_reserved = args.get_u64("reserved", 8192);
    spec.paths = args.get_size("paths", 8);
    spec.path_resources = args.get_size("path-resources", 2);
    spec.policy_complexity = args.get_size("complexity", 1);
    spec.protected_bp = static_cast<BasisPoints>(args.get_u64("protected-bp", 0));
    spec.abandon_bp = static_cast<BasisPoints>(args.get_u64("abandon-bp", 0));
    return spec;
}

Status ScenarioSpec::validate() const {
    EFG_TRY(population.validate());
    if (!policy_id.valid() || !policy_generation.valid()) {
        return make_status(StatusCode::InvalidArgument, "scenario policy identity is incomplete");
    }
    if (policy_validity_ticks == 0) {
        return make_status(StatusCode::InvalidArgument, "scenario policy validity must be positive");
    }
    return {};
}

ScenarioSpec scenario_from_args(const Args& args) {
    ScenarioSpec spec;
    spec.population = population_from_args(args);
    spec.policy_id = PolicyId{args.get_u64("policy-id", 7)};
    spec.policy_generation = Generation{args.get_u64("policy-generation", 1)};
    spec.policy_validity_ticks = args.get_u64("policy-validity", spec.population.ticks * 8u + 64u);
    return spec;
}

namespace {

constexpr Tick kScenarioStartTick = 1000;

Provenance make_provenance(const BootIdentity& boot, PublisherId publisher, u64 sequence, Tick at) {
    Provenance provenance;
    provenance.publisher.publisher = publisher;
    provenance.publisher.boot = boot.boot;
    provenance.publisher.epoch = boot.epoch;
    provenance.publisher.sequence = sequence;
    provenance.emitted_at = at;
    provenance.schema_version = kSchemaVersion;
    return provenance;
}

}  // namespace

std::vector<ResourceCapacity> build_capacities(const ScenarioSpec& spec, Tick at,
                                               const BootIdentity& boot) {
    std::vector<ResourceCapacity> out;
    out.reserve(spec.population.resources);
    StatusOr<ValidityWindow> validity =
        make_validity(at, spec.policy_validity_ticks + spec.population.ticks * 4u + 64u);
    for (std::size_t r = 0; r < spec.population.resources; ++r) {
        ResourceCapacity capacity;
        capacity.resource = ResourceId{r + 1};
        capacity.generation = Generation::initial();
        capacity.snapshot = CapacitySnapshotId{r + 1};
        capacity.span = TickSpan{at, at + spec.population.ticks + 1};
        capacity.capacity = spec.population.resource_capacity * kKiloTickScale;
        capacity.reserved = spec.population.resource_reserved * kKiloTickScale;
        if (validity.ok()) {
            capacity.validity = validity.value();
        }
        // The coordinator is its own publisher for fabric capacity evidence.
        capacity.provenance = make_provenance(boot, PublisherId{1}, r + 1, at);
        out.push_back(capacity);
    }
    return out;
}

std::vector<PathDescriptor> build_paths(const ScenarioSpec& spec, Tick at,
                                        const BootIdentity& boot) {
    std::vector<PathDescriptor> out;
    out.reserve(spec.population.paths);
    StatusOr<ValidityWindow> validity =
        make_validity(at, spec.policy_validity_ticks + spec.population.ticks * 4u + 64u);
    for (std::size_t p = 0; p < spec.population.paths; ++p) {
        PathDescriptor path;
        path.path = PathId{p + 1};
        path.generation = Generation::initial();
        for (std::size_t k = 0; k < spec.population.path_resources; ++k) {
            const std::size_t resource = (p + k) % spec.population.resources;
            path.resources.push_back(ResourceId{resource + 1});
        }
        if (validity.ok()) {
            path.validity = validity.value();
        }
        path.provenance = make_provenance(boot, PublisherId{1}, p + 2, at);
        out.push_back(path);
    }
    return out;
}

StatusOr<SampleStream> build_stream(const ScenarioSpec& spec, const BootIdentity& boot,
                                    PublisherId publisher, u64 shard, u64 shards) {
    EFG_TRY(spec.validate());
    if (shards == 0 || shard >= shards) {
        return make_status(StatusCode::InvalidArgument, "shard index is outside the shard count");
    }
    const std::size_t population = spec.population.mice + spec.population.elephants;
    const u64 window_count = spec.population.ticks / spec.population.window_ticks;
    const u64 window_seconds = spec.population.window_ticks;

    SampleStream stream;
    SeededRandom random(spec.population.seed + shard * 0x9E3779B97F4A7C15ull);

    for (std::size_t index = 0; index < population; ++index) {
        if (static_cast<u64>(index) % shards != shard) {
            continue;
        }
        const bool elephant = index >= spec.population.mice;
        const FlowId flow{index + 1};
        if (!stream.first_flow) {
            stream.first_flow = flow.value();
        }
        ++stream.flow_count;
        const PathId path{1 + (index % spec.population.paths)};
        u64 rate = 0;
        if (elephant) {
            rate = random.next_in(spec.population.elephant_rate_lo, spec.population.elephant_rate_hi);
        } else {
            rate = random.next_in(spec.population.mouse_rate_lo, spec.population.mouse_rate_hi);
        }
        const bool protected_flow = spec.population.protected_bp != 0 &&
                                    random.next_below(kBasisPointsScale) < spec.population.protected_bp;
        const bool abandons = spec.population.abandon_bp != 0 &&
                              random.next_below(kBasisPointsScale) < spec.population.abandon_bp;
        const u64 effective =
            abandons ? (window_count == 0 ? 1u : (window_count / 2u == 0 ? 1u : window_count / 2u))
                     : window_count;

        u64 cumulative = 0;
        for (u64 w = 0; w < effective; ++w) {
            const Tick begin = kScenarioStartTick + w * window_seconds;
            const Tick end = begin + window_seconds;
            const u64 window_bytes = rate * window_seconds;
            cumulative += window_bytes;

            FlowSample sample;
            sample.flow = flow;
            sample.flow_generation = Generation::initial();
            sample.path = path;
            sample.path_generation = Generation::initial();
            sample.window = EvidenceWindowId{index * (window_count == 0 ? 1u : window_count) + w + 1};
            sample.window_sequence = w + 1;
            sample.span = TickSpan{begin, end};
            sample.cumulative_bytes = cumulative;
            sample.window_bytes = window_bytes;
            sample.service_class = elephant ? ServiceClass::Standard : ServiceClass::BestEffort;
            sample.priority = Priority{static_cast<u8>(elephant ? 3 : 1)};
            if (protected_flow) {
                sample.protection = elephant ? ProtectionState::NonPreemptible
                                             : ProtectionState::Protected;
                sample.service_class = ServiceClass::Reserved;
                sample.reservation = ReservationId{1000 + index};
            } else {
                sample.protection = ProtectionState::Unprotected;
            }
            StatusOr<ValidityWindow> validity =
                make_validity(begin, spec.population.ticks * 4u + 64u);
            if (!validity.ok()) {
                return validity.status();
            }
            sample.validity = validity.value();
            sample.provenance =
                make_provenance(boot, publisher, index * 1000 + w + 1, begin);
            sample.provenance.digest = sample.digest();
            stream.samples.push_back(sample);
        }
    }
    return stream;
}

Status write_text_file(const std::string& path, const std::string& text) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return make_status(StatusCode::IoError, "file could not be created");
    }
    const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
    if (written != text.size()) {
        return make_status(StatusCode::IoError, "file write failed");
    }
    return {};
}

StatusOr<std::string> read_text_file(const std::string& path, std::size_t ceiling) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return make_status(StatusCode::NotFound, "file could not be opened");
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return make_status(StatusCode::IoError, "file seek failed");
    }
    const long size = std::ftell(file);
    if (size < 0) {
        std::fclose(file);
        return make_status(StatusCode::IoError, "file size could not be determined");
    }
    if (static_cast<std::size_t>(size) > ceiling) {
        std::fclose(file);
        return make_status(StatusCode::Oversized, "file exceeds the read ceiling");
    }
    std::rewind(file);
    std::string out(static_cast<std::size_t>(size), '\0');
    const std::size_t read = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    if (read != out.size()) {
        return make_status(StatusCode::IoError, "file read failed");
    }
    return out;
}

}  // namespace efg::cli
