#pragma once

#include <string>
#include <optional>
#include <format>

namespace cryptodd {

enum class ErrorCode {
    Unknown,
    DecompressionFailure,
    CompressionFailure,
    EncodingFailure,
    InvalidChunkShape,
    InvalidDataType,
    InvalidDataSize,
    CodecInternalError,
    InvalidStateSize,
};

[[nodiscard]] inline std::string_view to_string(const ErrorCode code) {
    switch (code) {
        case ErrorCode::Unknown: return "Unknown";
        case ErrorCode::DecompressionFailure: return "DecompressionFailure";
        case ErrorCode::CompressionFailure: return "CompressionFailure";
        case ErrorCode::EncodingFailure: return "EncodingFailure";
        case ErrorCode::InvalidChunkShape: return "InvalidChunkShape";
        case ErrorCode::InvalidDataType: return "InvalidDataType";
        case ErrorCode::InvalidDataSize: return "InvalidDataSize";
        case ErrorCode::CodecInternalError: return "CodecInternalError";
        case ErrorCode::InvalidStateSize: return "InvalidStateSize";
    }
    return "InvalidErrorCode";
}

class CodecError {
    ErrorCode code_ = ErrorCode::Unknown;
    std::optional<std::string> details_;

public:
    CodecError(ErrorCode code, std::optional<std::string> details) 
        : code_(code), details_(std::move(details)) {}

    explicit CodecError(ErrorCode code) : code_(code) {}

    // Helper to convert an unexpected from another library (like a codec)
    static CodecError from_string(std::string_view err_str, const ErrorCode err_code = ErrorCode::CodecInternalError) {
        return {err_code, std::string(err_str)};
    }

    [[nodiscard]] ErrorCode code() const { return code_; }
    [[nodiscard]] const std::optional<std::string>& details() const { return details_; }

    [[nodiscard]] std::string to_string() const {
        // CORRECTED: The to_string method must construct the string from its
        // members directly, not by calling std::format on itself. This
        // breaks the compile-time recursion.
        if (details_) {
            return std::format("CodecError(code={}, details=\"{}\")", cryptodd::to_string(code_), *details_);
        }
        return std::format("CodecError(code={})", cryptodd::to_string(code_));
    }
};

} // namespace cryptodd

// std::formatter specialization for easy printing/logging
template <>
struct std::formatter<cryptodd::CodecError> {
    // Add a simple parse function.
    // This is required by the formatter concept. It is called by the compiler
    // to validate the format specifiers inside the {}. Since we don't have
    // any, we just check for the end of the specifier (a '}') and return.
    constexpr auto parse(std::format_parse_context& ctx) {
        auto it = ctx.begin(), end = ctx.end();
        if (it != end && *it != '}') {
            throw std::format_error("invalid format specifier for CodecError");
        }
        return it;
    }

    // The format function now simply calls the object's to_string() method.
    // This avoids duplicating the formatting logic.
    auto format(const cryptodd::CodecError& err, format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", err.to_string());
    }
};