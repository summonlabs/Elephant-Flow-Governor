// Elephant Flow Governor - framed loopback transport implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/transport.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace efg {

std::string_view to_string(FrameType type) noexcept {
    switch (type) {
        case FrameType::Invalid: return "invalid";
        case FrameType::Hello: return "hello";
        case FrameType::Welcome: return "welcome";
        case FrameType::Fence: return "fence";
        case FrameType::Heartbeat: return "heartbeat";
        case FrameType::EvidenceBatch: return "evidence_batch";
        case FrameType::EvidenceAck: return "evidence_ack";
        case FrameType::FlowCompletion: return "flow_completion";
        case FrameType::PolicyPush: return "policy_push";
        case FrameType::CapacityPush: return "capacity_push";
        case FrameType::PathPush: return "path_push";
        case FrameType::IntentGrant: return "intent_grant";
        case FrameType::StatusReport: return "status_report";
        case FrameType::Shutdown: return "shutdown";
        case FrameType::Bye: return "bye";
        case FrameType::Failure: return "failure";
        case FrameType::Count: return "count";
    }
    return "invalid";
}

bool parse_frame_type(std::string_view text, FrameType& out) noexcept {
    for (u16 i = 0; i < static_cast<u16>(FrameType::Count); ++i) {
        const auto type = static_cast<FrameType>(i);
        if (to_string(type) == text) {
            out = type;
            return true;
        }
    }
    return false;
}

std::string_view to_string(FenceReason reason) noexcept {
    switch (reason) {
        case FenceReason::None: return "none";
        case FenceReason::EpochStale: return "epoch_stale";
        case FenceReason::EpochAhead: return "epoch_ahead";
        case FenceReason::BootMismatch: return "boot_mismatch";
        case FenceReason::WorkerUnknown: return "worker_unknown";
        case FenceReason::WorkerDuplicate: return "worker_duplicate";
        case FenceReason::FrameMalformed: return "frame_malformed";
        case FenceReason::FrameOversized: return "frame_oversized";
        case FenceReason::IntegrityFailure: return "integrity_failure";
        case FenceReason::SchemaMismatch: return "schema_mismatch";
        case FenceReason::GenerationRegression: return "generation_regression";
        case FenceReason::ShuttingDown: return "shutting_down";
        case FenceReason::PopulationCeiling: return "population_ceiling";
    }
    return "none";
}

// --- Frame codec ------------------------------------------------------------

Status FrameCodec::encode(const Frame& frame, ByteWriter& writer) const {
    if (frame.header.type >= FrameType::Count || frame.header.type == FrameType::Invalid) {
        return make_status(StatusCode::InvalidArgument, "frame type is not encodable");
    }
    if (frame.payload.size() > limits_.max_payload) {
        return make_status(StatusCode::Oversized, "frame payload exceeds the configured ceiling");
    }
    const u32 payload_crc = crc32c(frame.payload);
    writer.write_u32(kFrameMagic);
    writer.write_u32(kSchemaVersion);
    writer.write_u16(static_cast<u16>(frame.header.type));
    writer.write_u16(frame.header.flags);
    writer.write_u64(frame.header.sequence);
    writer.write_u64(frame.header.epoch.value());
    writer.write_u64(frame.header.boot.value());
    writer.write_u64(frame.header.worker.value());
    writer.write_u32(static_cast<u32>(frame.payload.size()));
    writer.write_u32(payload_crc);
    if (writer.size() != kFrameCrcOffset) {
        return make_status(StatusCode::Internal, "frame header layout drifted from the constant");
    }
    // The header CRC covers every header byte preceding it.
    std::span<const std::byte> header{writer.buffer().data(), writer.size()};
    writer.write_u32(crc32c(header));
    writer.write_bytes(frame.payload);
    if (writer.overflowed()) {
        return make_status(StatusCode::Oversized, "encoded frame exceeded the writer ceiling");
    }
    return {};
}

Status FrameCodec::decode_header(std::span<const std::byte> bytes, FrameHeader& out) const {
    if (bytes.size() < kFrameHeaderBytes) {
        return make_status(StatusCode::Truncated, "frame header is shorter than 56 bytes");
    }
    ByteReader reader(bytes.first(kFrameHeaderBytes));
    u32 magic = 0;
    u32 schema = 0;
    u16 type = 0;
    if (!reader.read_u32(magic) || !reader.read_u32(schema) || !reader.read_u16(type) ||
        !reader.read_u16(out.flags)) {
        return make_status(StatusCode::Truncated, "frame header prefix is truncated");
    }
    if (magic != kFrameMagic) {
        return make_status(StatusCode::Corrupt, "frame magic does not match");
    }
    if (schema != kSchemaVersion) {
        return make_status(StatusCode::VersionMismatch, "frame schema version is not supported");
    }
    if (type == 0 || type >= static_cast<u16>(FrameType::Count)) {
        return make_status(StatusCode::Corrupt, "frame type is not recognized");
    }
    out.type = static_cast<FrameType>(type);
    u64 epoch = 0;
    u64 boot = 0;
    u64 worker = 0;
    if (!reader.read_u64(out.sequence) || !reader.read_u64(epoch) || !reader.read_u64(boot) ||
        !reader.read_u64(worker) || !reader.read_u32(out.payload_length) ||
        !reader.read_u32(out.payload_crc)) {
        return make_status(StatusCode::Truncated, "frame header body is truncated");
    }
    out.epoch = EpochId{epoch};
    out.boot = BootId{boot};
    out.worker = WorkerId{worker};

    u32 header_crc = 0;
    if (!reader.read_u32(header_crc)) {
        return make_status(StatusCode::Truncated, "frame header checksum is truncated");
    }
    if (header_crc != crc32c(bytes.first(kFrameCrcOffset))) {
        return make_status(StatusCode::IntegrityFailure, "frame header checksum does not match");
    }
    if (out.payload_length > limits_.max_payload) {
        return make_status(StatusCode::Oversized, "frame payload length exceeds the ceiling");
    }
    return {};
}

Status FrameCodec::decode(std::span<const std::byte> bytes, Frame& out) const {
    EFG_TRY(decode_header(bytes, out.header));
    const std::size_t expected = kFrameHeaderBytes + static_cast<std::size_t>(out.header.payload_length);
    if (bytes.size() < expected) {
        return make_status(StatusCode::Truncated, "frame payload is shorter than declared");
    }
    const std::span<const std::byte> payload = bytes.subspan(kFrameHeaderBytes,
                                                             out.header.payload_length);
    if (crc32c(payload) != out.header.payload_crc) {
        return make_status(StatusCode::IntegrityFailure, "frame payload checksum does not match");
    }
    out.payload.assign(payload.begin(), payload.end());
    return {};
}

// --- Socket runtime ---------------------------------------------------------

namespace {

std::mutex g_runtime_mutex;
int g_runtime_users = 0;
bool g_runtime_ready = false;

}  // namespace

NetworkRuntime::NetworkRuntime() {
    std::lock_guard<std::mutex> guard(g_runtime_mutex);
#ifdef _WIN32
    if (g_runtime_users == 0) {
        WSADATA data;
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            g_runtime_ready = false;
            ok_ = false;
            status_ = make_status(StatusCode::IoError, "WSAStartup failed");
            return;
        }
        g_runtime_ready = true;
    }
    ok_ = g_runtime_ready;
    if (!ok_) {
        status_ = make_status(StatusCode::IoError, "socket runtime is not available");
    }
#else
    g_runtime_ready = true;
    ok_ = true;
#endif
    ++g_runtime_users;
}

NetworkRuntime::~NetworkRuntime() {
    std::lock_guard<std::mutex> guard(g_runtime_mutex);
    if (g_runtime_users > 0) {
        --g_runtime_users;
    }
#ifdef _WIN32
    if (g_runtime_users == 0 && g_runtime_ready) {
        WSACleanup();
        g_runtime_ready = false;
    }
#endif
}

namespace {

#ifdef _WIN32
using native_socket = SOCKET;
constexpr native_socket kBadSocket = INVALID_SOCKET;

Status last_socket_error(const char* what) {
    const int code = WSAGetLastError();
    if (code == WSAECONNRESET || code == WSAECONNABORTED) {
        return make_status(StatusCode::Closed, what);
    }
    return make_status(StatusCode::IoError, what);
}

Status close_socket(native_socket socket) {
    if (socket == kBadSocket) {
        return {};
    }
    if (closesocket(socket) != 0) {
        return last_socket_error("closesocket failed");
    }
    return {};
}

Status shutdown_socket(native_socket socket) {
    if (socket == kBadSocket) {
        return {};
    }
    if (shutdown(socket, SD_SEND) != 0) {
        return last_socket_error("shutdown failed");
    }
    return {};
}
#else
using native_socket = int;
constexpr native_socket kBadSocket = -1;

Status last_socket_error(const char* what) {
    return make_status(StatusCode::IoError, what);
}

Status close_socket(native_socket socket) {
    if (socket == kBadSocket) {
        return {};
    }
    if (::close(socket) != 0) {
        return last_socket_error("close failed");
    }
    return {};
}

Status shutdown_socket(native_socket socket) {
    if (socket == kBadSocket) {
        return {};
    }
    if (::shutdown(socket, SHUT_WR) != 0) {
        return last_socket_error("shutdown failed");
    }
    return {};
}
#endif

int send_flags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

}  // namespace

TcpStream::~TcpStream() {
    if (valid()) {
        close_socket(static_cast<native_socket>(socket_));
    }
}

TcpStream::TcpStream(TcpStream&& other) noexcept : socket_(other.socket_) {
    other.socket_ = kInvalidSocket;
}

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
    if (this != &other) {
        if (valid()) {
            close_socket(static_cast<native_socket>(socket_));
        }
        socket_ = other.socket_;
        other.socket_ = kInvalidSocket;
    }
    return *this;
}

Status TcpStream::read_exact(std::span<std::byte> out) {
    std::size_t offset = 0;
    while (offset < out.size()) {
        const int chunk = static_cast<int>(
            std::min<std::size_t>(out.size() - offset, static_cast<std::size_t>(0x40000000)));
        const int received =
            ::recv(static_cast<native_socket>(socket_), reinterpret_cast<char*>(out.data() + offset),
                   chunk, 0);
        if (received == 0) {
            return make_status(StatusCode::Closed, "peer closed the connection mid frame");
        }
        if (received < 0) {
            return last_socket_error("socket receive failed");
        }
        offset += static_cast<std::size_t>(received);
    }
    return {};
}

Status TcpStream::read_some(std::span<std::byte> out, std::size_t& received) {
    received = 0;
    if (out.empty()) {
        return make_status(StatusCode::InvalidArgument, "receive buffer is empty");
    }
    const int chunk = static_cast<int>(
        std::min<std::size_t>(out.size(), static_cast<std::size_t>(0x40000000)));
    const int result =
        ::recv(static_cast<native_socket>(socket_), reinterpret_cast<char*>(out.data()), chunk, 0);
    if (result == 0) {
        return make_status(StatusCode::Closed, "peer closed the connection");
    }
    if (result < 0) {
        return last_socket_error("socket receive failed");
    }
    received = static_cast<std::size_t>(result);
    return {};
}

Status TcpStream::write_all(std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const int chunk = static_cast<int>(
            std::min<std::size_t>(bytes.size() - offset, static_cast<std::size_t>(0x40000000)));
        const int sent = ::send(static_cast<native_socket>(socket_),
                                reinterpret_cast<const char*>(bytes.data() + offset), chunk,
                                send_flags());
        if (sent <= 0) {
            return last_socket_error("socket send failed");
        }
        offset += static_cast<std::size_t>(sent);
    }
    return {};
}

Status TcpStream::shutdown_send() { return shutdown_socket(static_cast<native_socket>(socket_)); }

Status TcpStream::close() {
    const Status status = close_socket(static_cast<native_socket>(socket_));
    socket_ = kInvalidSocket;
    return status;
}

std::string TcpStream::peer() const {
    if (!valid()) {
        return "closed";
    }
    sockaddr_in address{};
#ifdef _WIN32
    int length = static_cast<int>(sizeof(address));
#else
    socklen_t length = sizeof(address);
#endif
    if (getpeername(static_cast<native_socket>(socket_), reinterpret_cast<sockaddr*>(&address),
                    &length) != 0) {
        return "unknown";
    }
    const unsigned port = static_cast<unsigned>(ntohs(address.sin_port));
    std::string out = "127.0.0.1:";
    out.append(std::to_string(port));
    return out;
}

TcpListener::~TcpListener() {
    if (valid()) {
        close_socket(static_cast<native_socket>(socket_));
    }
}

TcpListener::TcpListener(TcpListener&& other) noexcept
    : socket_(other.socket_), bound_port_(other.bound_port_) {
    other.socket_ = kInvalidSocket;
    other.bound_port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        if (valid()) {
            close_socket(static_cast<native_socket>(socket_));
        }
        socket_ = other.socket_;
        bound_port_ = other.bound_port_;
        other.socket_ = kInvalidSocket;
        other.bound_port_ = 0;
    }
    return *this;
}

StatusOr<TcpListener> TcpListener::bind_loopback(u16 port) {
    static NetworkRuntime runtime;
    if (!runtime.ok()) {
        return runtime.status();
    }
    const native_socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kBadSocket) {
        return last_socket_error("socket creation failed");
    }
    const int reuse = 1;
    (void)setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                     static_cast<int>(sizeof(reuse)));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(socket, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
        (void)close_socket(socket);
        return last_socket_error("loopback bind failed");
    }
    if (::listen(socket, 64) != 0) {
        (void)close_socket(socket);
        return last_socket_error("listen failed");
    }
    sockaddr_in bound{};
#ifdef _WIN32
    int length = static_cast<int>(sizeof(bound));
#else
    socklen_t length = sizeof(bound);
#endif
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
        (void)close_socket(socket);
        return last_socket_error("getsockname failed");
    }

    TcpListener listener;
    listener.socket_ = static_cast<std::uintptr_t>(socket);
    listener.bound_port_ = ntohs(bound.sin_port);
    return listener;
}

StatusOr<TcpStream> TcpListener::accept() {
    if (!valid()) {
        return make_status(StatusCode::Closed, "listener is not bound");
    }
    sockaddr_in address{};
#ifdef _WIN32
    int length = static_cast<int>(sizeof(address));
#else
    socklen_t length = sizeof(address);
#endif
    const native_socket accepted = ::accept(static_cast<native_socket>(socket_),
                                            reinterpret_cast<sockaddr*>(&address), &length);
    if (accepted == kBadSocket) {
        return last_socket_error("accept failed");
    }
    const int no_delay = 1;
    (void)setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&no_delay), static_cast<int>(sizeof(no_delay)));
    return TcpStream(static_cast<std::uintptr_t>(accepted));
}

Status TcpListener::close() {
    const Status status = close_socket(static_cast<native_socket>(socket_));
    socket_ = kInvalidSocket;
    bound_port_ = 0;
    return status;
}

StatusOr<TcpStream> connect_loopback(u16 port) {
    static NetworkRuntime runtime;
    if (!runtime.ok()) {
        return runtime.status();
    }
    const native_socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kBadSocket) {
        return last_socket_error("socket creation failed");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(socket, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) !=
        0) {
        const Status status = last_socket_error("loopback connect failed");
        (void)close_socket(socket);
        return status;
    }
    const int no_delay = 1;
    (void)setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay),
                     static_cast<int>(sizeof(no_delay)));
    return TcpStream(static_cast<std::uintptr_t>(socket));
}

Status read_frame(TcpStream& stream, const FrameCodec& codec, Frame& out) {
    std::vector<std::byte> header(kFrameHeaderBytes);
    EFG_TRY(stream.read_exact(header));
    FrameHeader parsed;
    EFG_TRY(codec.decode_header(header, parsed));
    std::vector<std::byte> whole(kFrameHeaderBytes + static_cast<std::size_t>(parsed.payload_length));
    std::memcpy(whole.data(), header.data(), kFrameHeaderBytes);
    if (parsed.payload_length != 0) {
        std::span<std::byte> tail{whole.data() + kFrameHeaderBytes, parsed.payload_length};
        EFG_TRY(stream.read_exact(tail));
    }
    return codec.decode(whole, out);
}

Status write_frame(TcpStream& stream, const FrameCodec& codec, const Frame& frame) {
    ByteWriter writer(limits::kMaxFrameBytes);
    EFG_TRY(codec.encode(frame, writer));
    return stream.write_all(writer.buffer());
}

}  // namespace efg
