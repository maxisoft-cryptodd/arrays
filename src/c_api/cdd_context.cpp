#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>

#include "cdd_context.h"
#include "../data_io/data_reader.h"
#include "../data_io/data_writer.h"
#include "base64.h"
#include "../storage/file_backend.h"
#include "operations/json_serialization.h"
#include "operations/flush_handler.h"
#include "operations/inspect_handler.h"
#include "operations/load_chunks_handler.h"
#include "operations/metadata_handler.h"
#include "operations/operation_handler.h"
#include "operations/store_array_handler.h"
#include "operations/store_chunk_handler.h"
#include "operations/ping_handler.h"

namespace cryptodd::ffi {

CddContext::CddContext(
    ProtectedMarker,
    std::unique_ptr<DataReader>&& reader,
    std::unique_ptr<DataWriter>&& writer,
    std::string backend_type,
    std::string mode
) : reader_(std::move(reader)), writer_(std::move(writer)), compressor_(), extractor_(),
    backend_type_(std::move(backend_type)), mode_(std::move(mode)) {}

CddContext::~CddContext() {
    if (writer_) {
        if (auto flushed = writer_->flush(); !flushed)
        {
            std::cerr << "Error flushing data: " << flushed.error() << std::endl;
        }
    }
}

std::expected<std::unique_ptr<CddContext>, ExpectedError> CddContext::create(const nlohmann::json& config_json) {
    try {
        auto config_result = from_json<ContextConfig>(config_json);
        if (!config_result) {
            return std::unexpected(config_result.error());
        }
        const auto& config = *config_result;
        const auto& backend_config = config.backend;

        std::unique_ptr<DataReader> reader;
        std::unique_ptr<DataWriter> writer;

        if (backend_config.mode == "Read") {
            if (backend_config.type != "File") {
                return std::unexpected(ExpectedError("Read mode currently only supports File backend."));
            }
            if (!backend_config.path) {
                return std::unexpected(ExpectedError("File backend in Read mode requires a 'path'."));
            }
            auto reader_result = DataReader::open(*backend_config.path);
            if (!reader_result) {
                return std::unexpected(ExpectedError(reader_result.error()));
            }
            reader = std::move(*reader_result);
        } else { // WriteAppend or WriteTruncate
            size_t capacity = DataWriter::DEFAULT_CHUNK_OFFSETS_BLOCK_CAPACITY;
            memory::vector<std::byte> user_metadata;

            if (config.writer_options) {
                const auto& writer_opts = *config.writer_options;
                if (writer_opts.chunk_offsets_block_capacity) {
                    capacity = *writer_opts.chunk_offsets_block_capacity;
                }
                if (writer_opts.user_metadata_base64) {
                    const auto& metadata_b64 = *writer_opts.user_metadata_base64;
                    if (!metadata_b64.empty()) {
                        try {
                            user_metadata = base64::decode(metadata_b64);
                        } catch (const std::exception& e) {
                            return std::unexpected(ExpectedError(std::string("Failed to decode base64 user_metadata from config: ") + e.what()));
                        }
                    }
                }
            }

            std::expected<std::unique_ptr<DataWriter>, std::string> writer_result;
            if (backend_config.type == "File") {
                if (!backend_config.path) {
                    return std::unexpected(ExpectedError("File backend requires a 'path'."));
                }
                if (backend_config.mode == "WriteAppend") {
                    writer_result = DataWriter::open_for_append(*backend_config.path);
                } else { // WriteTruncate
                    writer_result = DataWriter::create_new(*backend_config.path, capacity, user_metadata);
                }
            } else if (backend_config.type == "Memory") {
                if (backend_config.mode != "WriteTruncate") {
                    return std::unexpected(ExpectedError("Memory backend only supports WriteTruncate mode."));
                }
                writer_result = DataWriter::create_in_memory(capacity, user_metadata);
            } else {
                 return std::unexpected(ExpectedError("Unsupported backend type for writing: " + backend_config.type));
            }

            if(!writer_result) {
                return std::unexpected(ExpectedError(writer_result.error()));
            }
            writer = std::move(*writer_result);
        }
        
        return std::make_unique<CddContext>(ProtectedMarker{}, std::move(reader), std::move(writer), backend_config.type, backend_config.mode);

    } catch(const nlohmann::json::exception& e) {
        return std::unexpected(ExpectedError(std::string("JSON configuration error: ") + e.what()));
    }
}

std::optional<std::reference_wrapper<DataWriter>> CddContext::get_writer() {
    if (writer_) {
        return std::make_optional(std::ref(*writer_));
    }
    return std::nullopt;
}

std::optional<std::reference_wrapper<DataReader>> CddContext::get_reader() {
    if (reader_) {
        return std::make_optional(std::ref(*reader_));
    }
    return std::nullopt;
}

std::span<const std::byte> CddContext::get_zero_state(size_t byte_size) {
    std::lock_guard lock(zero_state_cache_mutex_);
    auto it = zero_state_cache_.find(byte_size);
    if (it == zero_state_cache_.end()) {
        it = zero_state_cache_.try_emplace(byte_size, byte_size, std::byte{0}).first;
    }
    return it->second;
}

std::expected<nlohmann::json, ExpectedError> CddContext::execute_operation(
    const nlohmann::json& op_request,
    std::span<const std::byte> input_data,
    std::span<std::byte> output_data)
{
    // NEW: Create the guard at the top of the function.
    ConcurrencyGuard guard(in_use_);
    if (!guard) {
        return std::unexpected(ExpectedError("Concurrent operation detected on the same context handle. Contexts are not thread-safe."));
    }

    // Using a static dispatch map is more efficient and scalable than an if-else chain.
    using HandlerFactory = std::function<std::unique_ptr<IOperationHandler>()>;
    static const std::unordered_map<std::string, HandlerFactory> op_handlers = {
        {"StoreChunk",      []() { return std::make_unique<StoreChunkHandler>(); }},
        {"StoreArray",      []() { return std::make_unique<StoreArrayHandler>(); }},
        {"Inspect",         []() { return std::make_unique<InspectHandler>(); }},
        {"LoadChunks",      []() { return std::make_unique<LoadChunksHandler>(); }},
        {"GetUserMetadata", []() { return std::make_unique<GetUserMetadataHandler>(); }},
        {"SetUserMetadata", []() { return std::make_unique<SetUserMetadataHandler>(); }},
        {"Flush",           []() { return std::make_unique<FlushHandler>(); }},
        {"Ping",            []() { return std::make_unique<PingHandler>(); }}
    };

    try {
        const std::string op_type = op_request.at("op_type").get<std::string>();

        auto it = op_handlers.find(op_type);
        if (it == op_handlers.end()) {
            return std::unexpected(ExpectedError("Unknown or unsupported op_type: " + op_type));
        }

        auto handler = it->second();
        return handler->execute(*this, op_request, input_data, output_data);

    } catch(const nlohmann::json::exception& e) {
        return std::unexpected(ExpectedError(std::string("JSON request error: ") + e.what()));
    }
}

} // namespace cryptodd::ffi
