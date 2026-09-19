// Elephant Flow Governor - framed transport.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Real OS sockets on loopback, with a real length prefixed framing layer. Every
// frame carries the epoch, boot and worker identity that produced it and its own
// CRC-32C, so a truncated, duplicated, reordered or stale frame is rejected
// before it can influence a decision.

#ifndef EFG_TRANSPORT_HPP
#define EFG_TRANSPORT_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "efg/checked.hpp"
#include "efg/hash.hpp"
#include "efg/identity.hpp"
#include "efg/limits.hpp"
#include "efg/status.hpp"

namespace efg {

enum class FrameType : std::uint16_t {
    Invalid = 0,
    Hello = 1,
    Welcome = 2,
    Fence = 3,
    Heartbeat = 4,
    EvidenceBatch = 5,
    EvidenceAck = 6,
    FlowCompletion = 7,
    PolicyPush = 8,
    CapacityPush = 9,
    PathPush = 10,
    IntentGrant = 11,
    StatusReport = 12,
    Shutdown = 13,
    Bye = 14,
    Failure = 15,
    Count = 16,
};

[[nodiscard]] std::string_view to_string(FrameType type) noexcept;
[[nodiscard]] bool parse_frame_type(std::string_view text, FrameType& out) noexcept;

/// Why a peer was fenced. Fencing is terminal for the session: the connection is
/// closed and no further frame from that peer is accepted.
enum class FenceReason : std::uint16_t {
    None = 0,
    EpochStale = 1,
    EpochAhead = 2,
    BootMismatch = 3,
    WorkerUnknown = 4,
    WorkerDuplicate = 5,
    FrameMalformed = 6,
    FrameOversized = 7,
    IntegrityFailure = 8,
    SchemaMismatch = 9,
    GenerationRegression = 10,
    ShuttingDown = 11,
    PopulationCeiling = 12,
};

[[nodiscard]] std::string_view to_string(FenceReason reason) noexcept;

inline constexpr u32 kFrameMagic = 0x45464746u;  // "EFGF"
inline constexpr std::size_t kFrameHeaderBytes = 56;
inline constexpr std::size_t kFrameCrcOffset = 52;

struct FrameHeader {
    FrameType type{FrameType::Invalid};
    u16 flags{0};
    u64 sequence{0};
    EpochId epoch{};
    BootId boot{};
    WorkerId worker{};
    u32 payload_length{0};
    u32 payload_crc{0};
};

struct Frame {
    FrameHeader header{};
    std::vector<std::byte> payload{};

    [[nodiscard]] std::size_t wire_size() const noexcept {
        return kFrameHeaderBytes + payload.size();
    }
};

/// Framing codec. Encoding always produces exactly one self describing frame.
class FrameCodec {
public:
    struct Limits {
        std::size_t max_payload{limits::kMaxFramePayload};
    };

    FrameCodec() = default;
    explicit FrameCodec(Limits limits) : limits_(limits) {}

    [[nodiscard]] const Limits& limits() const noexcept { return limits_; }

    [[nodiscard]] Status encode(const Frame& frame, ByteWriter& writer) const;

    /// Parse a 56 byte header. The header CRC is verified here so that a caller
    /// can decide whether to read the payload before allocating for it.
    [[nodiscard]] Status decode_header(std::span<const std::byte> bytes, FrameHeader& out) const;

    /// Parse a complete frame (header plus payload).
    [[nodiscard]] Status decode(std::span<const std::byte> bytes, Frame& out) const;

private:
    Limits limits_{};
};

/// Process-wide socket runtime lifetime. Reference counted so that nested users
/// inside one process cannot tear the runtime down underneath each other.
class NetworkRuntime {
public:
    NetworkRuntime();
    ~NetworkRuntime();

    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] const Status& status() const noexcept { return status_; }

private:
    bool ok_{false};
    Status status_{};
};

/// A connected, blocking, loopback stream. No timeout is ever applied: a peer
/// that dies causes the operating system to report end of stream, and a peer that
/// stalls is a defect rather than something to paper over with a watchdog.
class TcpStream {
public:
    TcpStream() = default;
    ~TcpStream();

    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return socket_ != kInvalidSocket; }

    /// Read exactly count bytes. Returns Closed on orderly end of stream and
    /// IoError on a reset connection.
    Status read_exact(std::span<std::byte> out);

    /// Read at least one byte, at most out.size(). A zero result means the peer
    /// closed the connection.
    Status read_some(std::span<std::byte> out, std::size_t& received);

    Status write_all(std::span<const std::byte> bytes);

    Status shutdown_send();
    Status close();

    [[nodiscard]] std::string peer() const;

    /// Adopt an already connected native socket handle. Public so that the
    /// loopback connect helper can construct a stream, but not part of the
    /// supported surface: the handle must be a valid, exclusively owned socket.
    static constexpr std::uintptr_t kInvalidSocket = static_cast<std::uintptr_t>(~0ull);
    explicit TcpStream(std::uintptr_t socket) noexcept : socket_(socket) {}

private:
    std::uintptr_t socket_{kInvalidSocket};
};

class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    /// Bind an IPv4 loopback listener. Passing port 0 requests an ephemeral port;
    /// the chosen port is reported through bound_port().
    [[nodiscard]] static StatusOr<TcpListener> bind_loopback(u16 port);

    [[nodiscard]] bool valid() const noexcept { return socket_ != kInvalidSocket; }
    [[nodiscard]] u16 bound_port() const noexcept { return bound_port_; }

    /// Accept one connection, blocking until a peer arrives or the listener fails.
    [[nodiscard]] StatusOr<TcpStream> accept();

    Status close();

private:
    static constexpr std::uintptr_t kInvalidSocket = static_cast<std::uintptr_t>(~0ull);
    std::uintptr_t socket_{kInvalidSocket};
    u16 bound_port_{0};
};

/// Read one complete frame from a stream.
[[nodiscard]] Status read_frame(TcpStream& stream, const FrameCodec& codec, Frame& out);

/// Write one complete frame to a stream.
[[nodiscard]] Status write_frame(TcpStream& stream, const FrameCodec& codec, const Frame& frame);

/// Connect to a loopback listener.
[[nodiscard]] StatusOr<TcpStream> connect_loopback(u16 port);

}  // namespace efg

#endif  // EFG_TRANSPORT_HPP
