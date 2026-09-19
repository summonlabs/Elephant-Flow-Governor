// Elephant Flow Governor - status rendering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/status.hpp"

#include <string>

namespace efg {

std::string_view to_string(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::Ok: return "Ok";
        case StatusCode::InvalidArgument: return "InvalidArgument";
        case StatusCode::OutOfRange: return "OutOfRange";
        case StatusCode::Overflow: return "Overflow";
        case StatusCode::Underflow: return "Underflow";
        case StatusCode::DivideByZero: return "DivideByZero";
        case StatusCode::NotFound: return "NotFound";
        case StatusCode::AlreadyExists: return "AlreadyExists";
        case StatusCode::Conflict: return "Conflict";
        case StatusCode::CapacityExceeded: return "CapacityExceeded";
        case StatusCode::Unsupported: return "Unsupported";
        case StatusCode::Stale: return "Stale";
        case StatusCode::Expired: return "Expired";
        case StatusCode::UnknownEvidence: return "UnknownEvidence";
        case StatusCode::ContradictoryEvidence: return "ContradictoryEvidence";
        case StatusCode::DuplicateEvidence: return "DuplicateEvidence";
        case StatusCode::GapDetected: return "GapDetected";
        case StatusCode::GenerationMismatch: return "GenerationMismatch";
        case StatusCode::AuthorityMismatch: return "AuthorityMismatch";
        case StatusCode::EpochMismatch: return "EpochMismatch";
        case StatusCode::BootMismatch: return "BootMismatch";
        case StatusCode::RevalidationRequired: return "RevalidationRequired";
        case StatusCode::PolicyViolation: return "PolicyViolation";
        case StatusCode::ProtectionViolation: return "ProtectionViolation";
        case StatusCode::Corrupt: return "Corrupt";
        case StatusCode::Truncated: return "Truncated";
        case StatusCode::Oversized: return "Oversized";
        case StatusCode::VersionMismatch: return "VersionMismatch";
        case StatusCode::IntegrityFailure: return "IntegrityFailure";
        case StatusCode::IoError: return "IoError";
        case StatusCode::Closed: return "Closed";
        case StatusCode::Cancelled: return "Cancelled";
        case StatusCode::ShuttingDown: return "ShuttingDown";
        case StatusCode::Busy: return "Busy";
        case StatusCode::Internal: return "Internal";
    }
    return "Unknown";
}

std::string Status::to_string() const {
    std::string out{efg::to_string(code_)};
    if (!message_.empty()) {
        out.append(": ");
        out.append(message_);
    }
    return out;
}

}  // namespace efg
