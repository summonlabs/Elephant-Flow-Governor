// Elephant Flow Governor - flow evidence implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/flow.hpp"

#include <algorithm>
#include <limits>

namespace efg {

std::string_view to_string(ServiceClass value) noexcept {
    switch (value) {
        case ServiceClass::Unknown: return "unknown";
        case ServiceClass::BestEffort: return "best_effort";
        case ServiceClass::Standard: return "standard";
        case ServiceClass::Priority: return "priority";
        case ServiceClass::Reserved: return "reserved";
        case ServiceClass::Control: return "control";
    }
    return "unknown";
}

bool parse_service_class(std::string_view text, ServiceClass& out) noexcept {
    if (text == "unknown") { out = ServiceClass::Unknown; return true; }
    if (text == "best_effort") { out = ServiceClass::BestEffort; return true; }
    if (text == "standard") { out = ServiceClass::Standard; return true; }
    if (text == "priority") { out = ServiceClass::Priority; return true; }
    if (text == "reserved") { out = ServiceClass::Reserved; return true; }
    if (text == "control") { out = ServiceClass::Control; return true; }
    return false;
}

std::string_view to_string(ProtectionState value) noexcept {
    switch (value) {
        case ProtectionState::Unknown: return "unknown";
        case ProtectionState::Unprotected: return "unprotected";
        case ProtectionState::Protected: return "protected";
        case ProtectionState::NonPreemptible: return "non_preemptible";
    }
    return "unknown";
}

bool parse_protection_state(std::string_view text, ProtectionState& out) noexcept {
    if (text == "unknown") { out = ProtectionState::Unknown; return true; }
    if (text == "unprotected") { out = ProtectionState::Unprotected; return true; }
    if (text == "protected") { out = ProtectionState::Protected; return true; }
    if (text == "non_preemptible") { out = ProtectionState::NonPreemptible; return true; }
    return false;
}

namespace {

/// Canonical digest of a sample with the provenance digest field forced to zero,
/// so that the published digest is not self referential.
u64 sample_digest(const FlowSample& sample) {
    Hasher hasher;
    hasher.write_u64(sample.flow.value());
    hasher.write_u64(sample.flow_generation.value());
    hasher.write_u64(sample.path.value());
    hasher.write_u64(sample.path_generation.value());
    hasher.write_u64(sample.window.value());
    hasher.write_u64(sample.window_sequence);
    hasher.write_u64(sample.span.begin);
    hasher.write_u64(sample.span.end);
    hasher.write_u64(sample.cumulative_bytes);
    hasher.write_u64(sample.window_bytes);
    hasher.write_u8(static_cast<u8>(sample.service_class));
    hasher.write_bool(sample.priority.known());
    hasher.write_u8(sample.priority.level());
    hasher.write_u8(static_cast<u8>(sample.protection));
    hasher.write_u64(sample.reservation.value());
    hasher.write_u64(sample.tenant.value());
    hasher.write_u64(sample.validity.issued_at);
    hasher.write_u64(sample.validity.expires_at);
    hasher.write_u64(sample.provenance.publisher.publisher.value());
    hasher.write_u64(sample.provenance.publisher.boot.value());
    hasher.write_u64(sample.provenance.publisher.epoch.value());
    hasher.write_u64(sample.provenance.publisher.sequence);
    hasher.write_u64(sample.provenance.emitted_at);
    hasher.write_u32(sample.provenance.schema_version);
    return hasher.finish();
}

}  // namespace

u64 FlowSample::digest() const { return sample_digest(*this); }

Status FlowSample::validate() const {
    if (!flow.valid()) {
        return make_status(StatusCode::InvalidArgument, "flow identifier is absent");
    }
    if (!flow_generation.valid()) {
        return make_status(StatusCode::InvalidArgument, "flow generation is absent");
    }
    if (!path.valid()) {
        return make_status(StatusCode::InvalidArgument, "path identifier is absent");
    }
    if (!path_generation.valid()) {
        return make_status(StatusCode::InvalidArgument, "path generation is absent");
    }
    if (!window.valid()) {
        return make_status(StatusCode::InvalidArgument, "evidence window identifier is absent");
    }
    if (window_sequence == 0) {
        return make_status(StatusCode::InvalidArgument, "evidence window sequence must be positive");
    }
    if (span.end <= span.begin) {
        return make_status(StatusCode::InvalidArgument, "evidence span is empty or inverted");
    }
    if (window_bytes > cumulative_bytes) {
        return make_status(StatusCode::ContradictoryEvidence,
                           "window volume exceeds cumulative volume for the generation");
    }
    if (!validity.well_formed()) {
        return make_status(StatusCode::InvalidArgument, "evidence validity window is malformed");
    }
    if (validity.issued_at < span.begin) {
        return make_status(StatusCode::InvalidArgument,
                           "evidence validity starts before the observed span");
    }
    if (validity.expires_at <= span.end) {
        return make_status(StatusCode::Expired,
                           "evidence validity expires before the observed span ends");
    }
    if (priority.known() && priority.level() > Priority::kMaxLevel) {
        return make_status(StatusCode::OutOfRange, "priority level is outside the supported range");
    }
    if (!provenance.valid()) {
        return make_status(StatusCode::InvalidArgument, "provenance is incomplete");
    }
    if (provenance.schema_version != kSchemaVersion) {
        return make_status(StatusCode::VersionMismatch, "provenance schema version is not supported");
    }
    if (provenance.digest != sample_digest(*this)) {
        return make_status(StatusCode::IntegrityFailure, "evidence digest does not match content");
    }
    return {};
}

// --- FlowLedger -------------------------------------------------------------

FlowLedger::FlowLedger(FlowId flow, Generation generation, Limits limits)
    : flow_(flow), generation_(generation), limits_(limits) {
    if (limits_.max_samples == 0) {
        limits_.max_samples = 1;
    }
    if (limits_.max_samples > limits::kMaxSamplesPerFlow) {
        limits_.max_samples = limits::kMaxSamplesPerFlow;
    }
}

Status FlowLedger::check_order(const FlowSample& sample) const {
    if (!has_previous_) {
        return {};
    }
    if (sample.window_sequence <= last_sequence_) {
        return make_status(StatusCode::ContradictoryEvidence,
                           "evidence window sequence did not advance");
    }
    if (sample.span.begin < last_span_end_) {
        return make_status(StatusCode::ContradictoryEvidence,
                           "evidence windows overlap the previously accepted window");
    }
    if (sample.span.begin == last_span_end_) {
        const u64 delta = sample.cumulative_bytes - last_cumulative_;
        if (delta != sample.window_bytes) {
            return make_status(StatusCode::ContradictoryEvidence,
                               "contiguous window volume disagrees with the cumulative delta");
        }
    }
    return {};
}

StatusOr<AppendOutcome> FlowLedger::append(const FlowSample& sample) {
    if (sample.flow != flow_ || sample.flow_generation != generation_) {
        return make_status(StatusCode::GenerationMismatch,
                           "sample identity disagrees with the ledger it was routed to");
    }
    EFG_TRY(sample.validate());

    // A replay of the exact window that is already applied is idempotent. The
    // same window and sequence with different content is a contradiction.
    if (has_previous_ && sample.window_sequence == last_sequence_ && sample.window == last_window_) {
        if (samples_.back().digest() == sample.digest()) {
            return AppendOutcome::DuplicateIgnored;
        }
        return make_status(StatusCode::ContradictoryEvidence,
                           "same window and sequence with different content");
    }

    // Ordering inside the current measurement epoch is checked before the
    // epoch itself may be restarted, so an overlap can never be excused by a
    // later epoch change.
    EFG_TRY(check_order(sample));

    // Cumulative volume is monotone across the whole flow generation, even when
    // the flow moves to a different path generation.
    if (sample.cumulative_bytes < generation_cumulative_) {
        return make_status(StatusCode::ContradictoryEvidence,
                           "cumulative volume regressed inside one flow generation");
    }
    if (has_previous_ && sample.span.begin == last_span_end_ &&
        sample.cumulative_bytes < last_cumulative_) {
        return make_status(StatusCode::ContradictoryEvidence,
                           "cumulative volume regressed inside one measurement window");
    }

    // Every discontinuity starts a new measurement epoch. The retained windows
    // must form one contiguous run, otherwise a rate would be computed across a
    // boundary that no evidence covers.
    if (has_previous_) {
        const bool moved_path =
            sample.path != last_path_ || sample.path_generation != last_path_generation_;
        const bool discontinuous = sample.span.begin != last_span_end_;
        if (moved_path || discontinuous) {
            const bool telemetry_gap =
                discontinuous && !moved_path && limits_.max_gap_ticks != 0 &&
                sample.span.begin > last_span_end_ &&
                (sample.span.begin - last_span_end_) > limits_.max_gap_ticks;
            dropped_windows_ += samples_.size();
            samples_.clear();
            last_sequence_ = 0;
            last_window_ = {};
            last_span_end_ = 0;
            last_cumulative_ = 0;
            has_previous_ = false;
            gap_detected_ = telemetry_gap;
            if (moved_path) {
                path_changed_ = true;
            }
        }
    }

    if (limits_.max_bytes_per_window != 0 && sample.window_bytes > limits_.max_bytes_per_window) {
        return make_status(StatusCode::Oversized, "window volume exceeds the configured ceiling");
    }
    if (limits_.max_cumulative_bytes != 0 && sample.cumulative_bytes > limits_.max_cumulative_bytes) {
        return make_status(StatusCode::Oversized, "cumulative volume exceeds the configured ceiling");
    }

    if (samples_.size() >= limits_.max_samples) {
        // Evict the oldest retained window. The rate window narrows; it never
        // silently widens, and the eviction is counted so explanations can say so.
        samples_.erase(samples_.begin());
        ++dropped_windows_;
    }
    samples_.push_back(sample);

    last_sequence_ = sample.window_sequence;
    last_window_ = sample.window;
    last_span_end_ = sample.span.end;
    last_cumulative_ = sample.cumulative_bytes;
    last_path_ = sample.path;
    last_path_generation_ = sample.path_generation;
    generation_cumulative_ = sample.cumulative_bytes;
    has_previous_ = true;
    latest_validity_ = sample.validity;

    return AppendOutcome::Accepted;
}
Status FlowLedger::mark_completed(Tick completed_at) {
    if (completed_) {
        if (completed_at_ == completed_at) {
            return {};
        }
        return make_status(StatusCode::Conflict, "flow completion time changed after it was recorded");
    }
    if (has_previous_ && completed_at < last_span_end_) {
        return make_status(StatusCode::ContradictoryEvidence,
                           "completion precedes the last accepted evidence window");
    }
    completed_ = true;
    completed_at_ = completed_at;
    return {};
}

FlowMeasurements FlowLedger::measurements() const {
    FlowMeasurements out;
    const StatusOr<FlowMeasurements> derived =
        derive_measurements(samples_, limits_, completed_, gap_detected_, dropped_windows_);
    if (derived.ok()) {
        out = derived.value();
    }
    return out;
}

void FlowLedger::clear() noexcept {
    samples_.clear();
    latest_validity_ = {};
    last_sequence_ = 0;
    last_window_ = {};
    last_span_end_ = 0;
    last_cumulative_ = 0;
    last_path_ = {};
    last_path_generation_ = {};
    generation_cumulative_ = 0;
    has_previous_ = false;
    gap_detected_ = false;
    path_changed_ = false;
    completed_ = false;
    completed_at_ = 0;
    dropped_windows_ = 0;
}

StatusOr<FlowMeasurements> derive_measurements(std::span<const FlowSample> samples,
                                               const FlowLedger::Limits& limits,
                                               bool completed,
                                               bool gap_detected,
                                               u64 dropped_windows) {
    FlowMeasurements out;
    if (samples.empty()) {
        out.completed = completed;
        out.dropped_windows = dropped_windows;
        return out;
    }

    const FlowSample& first = samples.front();
    const FlowSample& last = samples.back();
    if (last.span.end < first.span.begin) {
        return make_status(StatusCode::ContradictoryEvidence, "evidence span ordering is inverted");
    }

    out.evidence_present = true;
    out.observed = TickSpan{first.span.begin, last.span.end};
    out.duration_ticks = out.observed.length();
    out.cumulative_bytes = last.cumulative_bytes;
    out.sample_count = static_cast<u64>(samples.size());
    out.window_count = static_cast<u64>(samples.size());
    out.dropped_windows = dropped_windows;
    out.gap_detected = gap_detected;
    out.completed = completed;
    out.service_class = last.service_class;
    out.priority = last.priority;
    out.protection = last.protection;
    out.reservation = last.reservation;
    out.has_reservation = last.reservation.valid();
    out.tenant = last.tenant;

    u64 window_sum = 0;
    // A rate is only a rate across at least two adjoining windows. A single
    // window is an observation, not a sustained measurement.
    bool contiguous = samples.size() >= 2;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        window_sum = saturating_add(window_sum, samples[i].window_bytes);
        const u64 length = samples[i].span.length();
        if (length == 0) {
            return make_status(StatusCode::ContradictoryEvidence, "evidence window has zero length");
        }
        u64 rate = 0;
        if (!mul_div(samples[i].window_bytes, kKiloTickScale, length, rate)) {
            rate = std::numeric_limits<u64>::max();
        }
        out.peak_window_rate = max_of(out.peak_window_rate, rate);
        if (i > 0 && samples[i].span.begin != samples[i - 1].span.end) {
            contiguous = false;
        }
    }
    out.retained_window_bytes = window_sum;
    out.evidence_contiguous = contiguous && !gap_detected;

    if (out.duration_ticks == 0) {
        return make_status(StatusCode::UnknownEvidence, "evidence span has zero duration");
    }
    u64 rate = 0;
    if (!mul_div(window_sum, kKiloTickScale, out.duration_ticks, rate)) {
        return make_status(StatusCode::Overflow, "sustained rate computation overflowed");
    }
    out.sustained_rate = rate;

    // When windows were dropped the retained rate window no longer covers the
    // whole observed duration, so recompute over the retained span only.
    if (dropped_windows != 0 && samples.size() >= 2) {
        const u64 retained = samples.back().span.end - samples.front().span.begin;
        if (retained != 0) {
            u64 retained_rate = 0;
            if (mul_div(window_sum, kKiloTickScale, retained, retained_rate)) {
                out.sustained_rate = retained_rate;
            }
        }
    }

    if (limits.max_gap_ticks != 0) {
        for (std::size_t i = 1; i < samples.size(); ++i) {
            if (samples[i].span.begin > samples[i - 1].span.end &&
                (samples[i].span.begin - samples[i - 1].span.end) > limits.max_gap_ticks) {
                out.gap_detected = true;
                out.evidence_contiguous = false;
            }
        }
    }

    out.fields_complete = out.evidence_present && !out.gap_detected &&
                          out.service_class != ServiceClass::Unknown &&
                          out.protection != ProtectionState::Unknown && out.priority.known();
    return out;
}

u64 FlowMeasurements::digest() const {
    Hasher hasher;
    hasher.write_bool(evidence_present);
    hasher.write_bool(evidence_contiguous);
    hasher.write_bool(gap_detected);
    hasher.write_bool(capacity_known);
    hasher.write_bool(path_known);
    hasher.write_u64(observed.begin);
    hasher.write_u64(observed.end);
    hasher.write_u64(duration_ticks);
    hasher.write_u64(cumulative_bytes);
    hasher.write_u64(retained_window_bytes);
    hasher.write_u64(sustained_rate);
    hasher.write_u64(peak_window_rate);
    hasher.write_u64(sample_count);
    hasher.write_u64(window_count);
    hasher.write_u64(dropped_windows);
    hasher.write_u8(static_cast<u8>(service_class));
    hasher.write_bool(priority.known());
    hasher.write_u8(priority.level());
    hasher.write_u8(static_cast<u8>(protection));
    hasher.write_u64(reservation.value());
    hasher.write_bool(has_reservation);
    hasher.write_u64(tenant.value());
    hasher.write_bool(completed);
    hasher.write_u64(binding_resource.value());
    hasher.write_u64(resource_capacity);
    hasher.write_u64(resource_reserved);
    hasher.write_u64(resource_available);
    hasher.write_u32(share_of_capacity);
    hasher.write_u32(share_of_available);
    hasher.write_u32(competing_flows);
    hasher.write_u32(competing_elephants);
    hasher.write_u64(excess_over_guarantee);
    hasher.write_bool(fields_complete);
    return hasher.finish();
}

}  // namespace efg
