// Elephant Flow Governor - synthetic population benchmark.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/benchmark.hpp"

#include <chrono>
#include <string>

#include "efg/text.hpp"

namespace efg {

Status PopulationSpec::validate() const {
    const std::size_t population = mice + elephants;
    if (population == 0) {
        return make_status(StatusCode::InvalidArgument, "population is empty");
    }
    if (population > limits::kMaxBenchmarkPopulation) {
        return make_status(StatusCode::Oversized, "population exceeds the benchmark ceiling");
    }
    if (ticks == 0 || window_ticks == 0) {
        return make_status(StatusCode::InvalidArgument, "tick horizon and window length must be positive");
    }
    if (window_ticks > ticks) {
        return make_status(StatusCode::InvalidArgument, "window length exceeds the tick horizon");
    }
    if (mouse_rate_lo == 0 || mouse_rate_hi < mouse_rate_lo) {
        return make_status(StatusCode::InvalidArgument, "mouse rate range is inverted");
    }
    if (elephant_rate_lo == 0 || elephant_rate_hi < elephant_rate_lo) {
        return make_status(StatusCode::InvalidArgument, "elephant rate range is inverted");
    }
    if (elephant_rate_lo <= mouse_rate_hi) {
        return make_status(StatusCode::InvalidArgument,
                           "elephant and mouse rate ranges overlap; the population is not separable");
    }
    if (resources == 0 || resource_capacity == 0) {
        return make_status(StatusCode::InvalidArgument, "capacity population must be non-empty");
    }
    if (resource_reserved >= resource_capacity) {
        return make_status(StatusCode::InvalidArgument, "reserved capacity leaves nothing available");
    }
    if (resource_reserved >= resource_capacity) {
        return make_status(StatusCode::InvalidArgument, "reserved capacity must leave headroom");
    }
    if (resources > limits::kMaxResources) {
        return make_status(StatusCode::Oversized, "resource population exceeds the ceiling");
    }
    if (paths == 0 || paths > limits::kMaxPaths) {
        return make_status(StatusCode::OutOfRange, "path population is outside the supported range");
    }
    if (path_resources == 0 || path_resources > resources) {
        return make_status(StatusCode::InvalidArgument,
                           "each path must bind at least one resource and no more than exist");
    }
    if (path_resources > limits::kMaxResourcesPerPath) {
        return make_status(StatusCode::Oversized, "path binds more resources than the ceiling allows");
    }
    if (policy_complexity == 0 || policy_complexity > 24) {
        return make_status(StatusCode::OutOfRange, "policy complexity is outside the supported range");
    }
    if (protected_bp > kBasisPointsScale || abandon_bp > kBasisPointsScale) {
        return make_status(StatusCode::OutOfRange, "population ratios exceed 10000 basis points");
    }
    return {};
}

namespace {

constexpr Tick kBenchmarkStartTick = 1000;

u64 rate_for(const PopulationSpec& spec, bool elephant, SeededRandom& random) {
    if (elephant) {
        return random.next_in(spec.elephant_rate_lo, spec.elephant_rate_hi);
    }
    return random.next_in(spec.mouse_rate_lo, spec.mouse_rate_hi);
}

u64 sustained_rate_threshold(const PopulationSpec& spec) {
    return (spec.mouse_rate_hi + spec.elephant_rate_lo) / 2u;
}

}  // namespace

StatusOr<PolicyDocument> make_benchmark_policy(const PopulationSpec& spec, PolicyId id,
                                                Generation generation, Tick issued_at,
                                                u64 validity_ticks) {
    EFG_TRY(spec.validate());
    PolicyDocument policy;
    policy.id = id;
    policy.generation = generation;

    const u64 rate_threshold_kilo = sustained_rate_threshold(spec) * kKiloTickScale;
    const u64 cumulative_threshold = spec.elephant_rate_lo * spec.window_ticks;
    const u64 duration_threshold = spec.window_ticks * 2u;
    const u32 share_threshold = 300u;

    RuleSet rules;
    rules.nodes.push_back(RuleNode{RuleKind::Threshold,
                                   Threshold{ThresholdKind::CumulativeBytes, cumulative_threshold,
                                             ServiceClass::Unknown},
                                   {}});
    rules.nodes.push_back(RuleNode{RuleKind::Threshold,
                                   Threshold{ThresholdKind::DurationTicks, duration_threshold,
                                             ServiceClass::Unknown},
                                   {}});
    rules.nodes.push_back(RuleNode{RuleKind::Threshold,
                                   Threshold{ThresholdKind::SustainedRate, rate_threshold_kilo,
                                             ServiceClass::Unknown},
                                   {}});
    rules.nodes.push_back(RuleNode{RuleKind::Threshold,
                                   Threshold{ThresholdKind::ShareOfCapacity, share_threshold,
                                             ServiceClass::Unknown},
                                   {}});
    for (std::size_t extra = 1; extra < spec.policy_complexity; ++extra) {
        rules.nodes.push_back(RuleNode{RuleKind::Threshold,
                                       Threshold{ThresholdKind::Always, 0, ServiceClass::Unknown},
                                       {}});
    }

    const std::size_t leaves = rules.nodes.size();
    u32 chain = 0;
    for (std::size_t i = 1; i < leaves; ++i) {
        RuleNode node;
        node.kind = RuleKind::All;
        node.children.push_back(chain);
        node.children.push_back(static_cast<u32>(i));
        rules.nodes.push_back(node);
        chain = static_cast<u32>(rules.nodes.size() - 1);
    }
    rules.root = chain;
    EFG_TRY(rules.validate());
    policy.enter_rule = rules;
    policy.exit_rule = RuleSet{};
    policy.hysteresis.enter_confirm_windows = 2;
    policy.hysteresis.exit_confirm_windows = 3;
    policy.hysteresis.exit_scale_bp = 8000;
    policy.authorization.allow_alternate_placement = true;
    policy.authorization.allow_rate_shaping = true;
    policy.authorization.allow_scheduling_isolation = true;
    policy.authorization.allow_competing_admission_reduction = true;
    policy.authorization.allow_congestion_escalation = true;
    policy.authorization.protect_obligations = true;
    policy.authorization.max_shaping_reduction_bp = 2500;
    policy.authorization.max_intent_duration_ticks = spec.ticks;
    policy.authorization.max_targets = 8;
    policy.authorization.min_impact_severity = 400;
    policy.authorization.min_contention_pressure = 200;
    StatusOr<ValidityWindow> validity = make_validity(issued_at, validity_ticks);
    if (!validity.ok()) {
        return validity.status();
    }
    policy.validity = validity.value();
    policy.provenance.publisher.publisher = PublisherId{1};
    policy.provenance.publisher.boot = BootId{1};
    policy.provenance.publisher.epoch = EpochId{1};
    policy.provenance.publisher.sequence = 1;
    policy.provenance.emitted_at = issued_at;
    policy.provenance.schema_version = kSchemaVersion;
    EFG_TRY(policy.finalize());
    EFG_TRY(policy.validate());
    return policy;
}

namespace {

StatusOr<u64> run_once(const PopulationSpec& spec, GovernorConfig governor_config,
                       BenchmarkResult& out) {
    Governor governor(governor_config);
    BootIdentity boot;
    boot.boot = BootId{1};
    boot.epoch = EpochId{1};
    boot.started_at = kBenchmarkStartTick;
    boot.monotonic_seed = spec.seed;
    governor.set_incarnation(boot);

    const Tick policy_validity = spec.ticks * 8u + 64u;
    StatusOr<PolicyDocument> policy =
        make_benchmark_policy(spec, PolicyId{7}, Generation::initial(), kBenchmarkStartTick,
                              policy_validity);
    if (!policy.ok()) {
        return policy.status();
    }
    EFG_TRY(governor.submit_policy(policy.value()));

    for (std::size_t r = 0; r < spec.resources; ++r) {
        ResourceCapacity capacity;
        capacity.resource = ResourceId{r + 1};
        capacity.generation = Generation::initial();
        capacity.snapshot = CapacitySnapshotId{r + 1};
        capacity.span = TickSpan{kBenchmarkStartTick, kBenchmarkStartTick + spec.ticks};
        capacity.capacity = spec.resource_capacity * kKiloTickScale;
        capacity.reserved = spec.resource_reserved * kKiloTickScale;
        StatusOr<ValidityWindow> validity = make_validity(kBenchmarkStartTick, policy_validity);
        if (!validity.ok()) {
            return validity.status();
        }
        capacity.validity = validity.value();
        capacity.provenance.publisher.publisher = PublisherId{1};
        capacity.provenance.publisher.boot = BootId{1};
        capacity.provenance.publisher.epoch = EpochId{1};
        capacity.provenance.publisher.sequence = r + 2;
        capacity.provenance.emitted_at = kBenchmarkStartTick;
        EFG_TRY(governor.submit_capacity(capacity));
    }

    for (std::size_t p = 0; p < spec.paths; ++p) {
        PathDescriptor path;
        path.path = PathId{p + 1};
        path.generation = Generation::initial();
        for (std::size_t k = 0; k < spec.path_resources; ++k) {
            const std::size_t resource = (p + k) % spec.resources;
            path.resources.push_back(ResourceId{resource + 1});
        }
        StatusOr<ValidityWindow> validity = make_validity(kBenchmarkStartTick, policy_validity);
        if (!validity.ok()) {
            return validity.status();
        }
        path.validity = validity.value();
        path.provenance.publisher.publisher = PublisherId{1};
        path.provenance.publisher.boot = BootId{1};
        path.provenance.publisher.epoch = EpochId{1};
        path.provenance.publisher.sequence = spec.resources + p + 2;
        path.provenance.emitted_at = kBenchmarkStartTick;
        EFG_TRY(governor.submit_path(path));
    }

    SeededRandom random(spec.seed);
    const std::size_t population = spec.mice + spec.elephants;
    const u64 window_count = spec.ticks / spec.window_ticks;

    for (std::size_t index = 0; index < population; ++index) {
        const bool elephant = index >= spec.mice;
        const FlowId flow{index + 1};
        const Generation generation = Generation::initial();
        const PathId path{1 + (index % spec.paths)};
        const u64 rate = rate_for(spec, elephant, random);

        const bool protected_flow =
            spec.protected_bp != 0 && random.next_below(kBasisPointsScale) < spec.protected_bp;
        const bool abandons =
            spec.abandon_bp != 0 && random.next_below(kBasisPointsScale) < spec.abandon_bp;
        const u64 effective_windows = abandons ? (window_count / 2u == 0 ? 1u : window_count / 2u)
                                               : window_count;
        (void)effective_windows;

        u64 cumulative = 0;
        for (u64 w = 0; w < effective_windows; ++w) {
            const Tick begin = kBenchmarkStartTick + w * spec.window_ticks;
            const Tick end = begin + spec.window_ticks;
            const u64 window_bytes = rate * spec.window_ticks;
            cumulative += window_bytes;

            FlowSample sample;
            sample.flow = flow;
            sample.flow_generation = generation;
            sample.path = path;
            sample.path_generation = Generation::initial();
            sample.window = EvidenceWindowId{index * window_count + w + 1};
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
            StatusOr<ValidityWindow> validity = make_validity(begin, spec.ticks * 4u + 64u);
            if (!validity.ok()) {
                return validity.status();
            }
            sample.validity = validity.value();
            sample.provenance.publisher.publisher = PublisherId{1};
            sample.provenance.publisher.boot = BootId{1};
            sample.provenance.publisher.epoch = EpochId{1};
            sample.provenance.publisher.sequence = index * window_count + w + 1;
            sample.provenance.emitted_at = begin;
            sample.provenance.schema_version = kSchemaVersion;
            sample.provenance.digest = sample.digest();

            StatusOr<FlowDecision> decision = governor.submit_evidence(sample);
            ++out.samples_submitted;
            if (decision.ok()) {
                ++out.samples_accepted;
                if (decision.value().evidence_duplicate) {
                    ++out.samples_duplicate;
                }
                ++out.decisions;
                switch (decision.value().classification.state) {
                    case ElephantState::Elephant: ++out.classified_elephant; break;
                    case ElephantState::NotElephant: ++out.classified_not_elephant; break;
                    case ElephantState::Unknown: ++out.classified_unknown; break;
                    case ElephantState::Suspended: ++out.classified_suspended; break;
                }
            } else {
                ++out.samples_rejected;
            }
        }
    }

    const Tick final_tick =
        kBenchmarkStartTick +
        (spec.sweep_after_ticks == 0 ? spec.ticks + 1 : spec.sweep_after_ticks);
    StatusOr<DecisionBatch> batch = governor.tick(final_tick);
    if (batch.ok()) {
        ++out.ticks;
    }

    for (const auto& key : governor.flow_keys()) {
        StatusOr<FlowClassification> classification =
            governor.classification(key.first, key.second, final_tick);
        if (!classification.ok()) {
            continue;
        }
        switch (classification.value().state) {
            case ElephantState::Elephant: ++out.final_elephants; break;
            case ElephantState::NotElephant: ++out.final_not_elephants; break;
            case ElephantState::Unknown: ++out.final_unknown; break;
            case ElephantState::Suspended: ++out.final_suspended; break;
        }
    }

    for (const GovernanceIntent& intent : governor.intents().all_intents()) {
        switch (intent.state) {
            case IntentState::Authorized:
            case IntentState::Active:
                ++out.intents_authorized;
                break;
            case IntentState::Clamped:
                ++out.intents_clamped;
                break;
            case IntentState::Suppressed:
                ++out.intents_suppressed;
                break;
            case IntentState::Fenced:
                ++out.intents_fenced;
                break;
            case IntentState::Revoked:
            case IntentState::Expired:
            case IntentState::Completed:
                ++out.intents_revoked;
                break;
        }
    }

    out.flows = population;
    out.mice = spec.mice;
    out.elephants = spec.elephants;
    out.counters = governor.counters();
    return StatusOr<u64>(decision_digest(governor).value_or(0));
}

}  // namespace

StatusOr<BenchmarkResult> run_benchmark(const PopulationSpec& spec, GovernorConfig governor_config) {
    EFG_TRY(spec.validate());

    BenchmarkResult result;
    const auto start = std::chrono::steady_clock::now();
    StatusOr<u64> digest = run_once(spec, governor_config, result);
    if (!digest.ok()) {
        return digest.status();
    }
    const auto finish = std::chrono::steady_clock::now();
    result.decision_digest = digest.value();

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count();
    result.elapsed_micros = elapsed < 0 ? 0 : static_cast<u64>(elapsed);

    // Determinism proof: the same synthetic population replayed into a fresh
    // governor must reproduce the identical decision digest.
    BenchmarkResult replay;
    StatusOr<u64> replay_digest = run_once(spec, governor_config, replay);
    if (!replay_digest.ok()) {
        return replay_digest.status();
    }
    result.second_run_digest = replay_digest.value();
    result.deterministic_replay = result.second_run_digest == result.decision_digest;

    if (result.elapsed_micros != 0) {
        const double seconds = static_cast<double>(result.elapsed_micros) / 1'000'000.0;
        result.classifications_per_second = static_cast<double>(result.decisions) / seconds;
        result.samples_per_second = static_cast<double>(result.samples_accepted) / seconds;
    }
    return result;
}

namespace {

constexpr char kNewline = static_cast<char>(10);

void field(std::string& out, const char* name, u64 value, bool last) {
    out.append("  \"");
    out.append(name);
    out.append("\": ");
    append_u64(out, value);
    out.append(last ? "" : ",");
    out.push_back(kNewline);
}

}  // namespace

std::string render_benchmark_json(const BenchmarkResult& result) {
    std::string out;
    out.reserve(4096);
    out.append("{");
    out.push_back(kNewline);
    out.append("  \"population_kind\": \"SYNTHETIC\",");
    out.push_back(kNewline);
    out.append("  \"physical_network\": false,");
    out.push_back(kNewline);
    field(out, "flows", result.flows, false);
    field(out, "mice", result.mice, false);
    field(out, "elephants", result.elephants, false);
    field(out, "samples_submitted", result.samples_submitted, false);
    field(out, "samples_accepted", result.samples_accepted, false);
    field(out, "samples_duplicate", result.samples_duplicate, false);
    field(out, "samples_rejected", result.samples_rejected, false);
    field(out, "decisions", result.decisions, false);
    field(out, "classified_elephant", result.classified_elephant, false);
    field(out, "classified_not_elephant", result.classified_not_elephant, false);
    field(out, "classified_unknown", result.classified_unknown, false);
    field(out, "classified_suspended", result.classified_suspended, false);
    field(out, "final_elephants", result.final_elephants, false);
    field(out, "final_not_elephants", result.final_not_elephants, false);
    field(out, "final_unknown", result.final_unknown, false);
    field(out, "final_suspended", result.final_suspended, false);
    field(out, "intents_authorized", result.intents_authorized, false);
    field(out, "intents_clamped", result.intents_clamped, false);
    field(out, "intents_suppressed", result.intents_suppressed, false);
    field(out, "intents_fenced", result.intents_fenced, false);
    field(out, "intents_revoked", result.intents_revoked, false);
    field(out, "ticks", result.ticks, false);
    field(out, "elapsed_micros", result.elapsed_micros, false);
    out.append("  \"classifications_per_second\": ");
    append_json_number(out, result.classifications_per_second, 2);
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"samples_per_second\": ");
    append_json_number(out, result.samples_per_second, 2);
    out.append(",");
    out.push_back(kNewline);
    field(out, "decision_digest", result.decision_digest, false);
    field(out, "second_run_digest", result.second_run_digest, false);
    out.append("  \"deterministic_replay\": ");
    out.append(result.deterministic_replay ? "true" : "false");
    out.push_back(kNewline);
    out.append("}");
    out.push_back(kNewline);
    return out;
}

std::string render_benchmark_text(const BenchmarkResult& result) {
    std::string out;
    out.reserve(2048);
    out.append("population: SYNTHETIC (no physical network was exercised)");
    out.push_back(kNewline);
    out.append("flows=");
    append_u64(out, result.flows);
    out.append(" mice=");
    append_u64(out, result.mice);
    out.append(" elephants=");
    append_u64(out, result.elephants);
    out.push_back(kNewline);
    out.append("samples submitted=");
    append_u64(out, result.samples_submitted);
    out.append(" accepted=");
    append_u64(out, result.samples_accepted);
    out.append(" duplicate=");
    append_u64(out, result.samples_duplicate);
    out.append(" rejected=");
    append_u64(out, result.samples_rejected);
    out.push_back(kNewline);
    out.append("decisions=");
    append_u64(out, result.decisions);
    out.append(" elephant=");
    append_u64(out, result.classified_elephant);
    out.append(" not_elephant=");
    append_u64(out, result.classified_not_elephant);
    out.append(" unknown=");
    append_u64(out, result.classified_unknown);
    out.append(" suspended=");
    append_u64(out, result.classified_suspended);
    out.append(" (decision counts)");
    out.push_back(kNewline);
    out.append("flows final: elephants=");
    append_u64(out, result.final_elephants);
    out.append(" not_elephants=");
    append_u64(out, result.final_not_elephants);
    out.append(" unknown=");
    append_u64(out, result.final_unknown);
    out.append(" suspended=");
    append_u64(out, result.final_suspended);
    out.push_back(kNewline);
    out.append("elapsed_us=");
    append_u64(out, result.elapsed_micros);
    out.append(" classifications_per_second=");
    append_json_number(out, result.classifications_per_second, 2);
    out.append(" samples_per_second=");
    append_json_number(out, result.samples_per_second, 2);
    out.push_back(kNewline);
    out.append("decision_digest=");
    append_u64(out, result.decision_digest);
    out.append(" replay_digest=");
    append_u64(out, result.second_run_digest);
    out.append(" deterministic=");
    out.append(result.deterministic_replay ? "true" : "false");
    out.push_back(kNewline);
    return out;
}

}  // namespace efg
