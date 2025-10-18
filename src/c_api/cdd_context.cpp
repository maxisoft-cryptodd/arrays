#include "cdd_context.h"
#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>
#include "../data_io/data_reader.h"
#include "../data_io/data_writer.h"
#include "../storage/file_backend.h"
#include "../storage/memory_backend.h"
#include "operations/flush_handler.h"
#include "operations/inspect_handler.h"
#include "operations/load_chunks_handler.h"
#include "operations/metadata_handler.h"
#include "operations/operation_handler.h"
#include "operations/store_array_handler.h"
#include "operations/store_chunk_handler.h"

namespace cryptodd::ffi {

CddContext::CddContext(
    ProtectedMarker,
    std::unique_ptr<DataReader>&& reader,
    std::unique_ptr<DataWriter>&& writer
) : reader_(std::move(reader)), writer_(std::move(writer)), compressor_(), extractor_() {}

CddContext::~CddContext() {
    if (writer_) {
        if (auto flushed = writer_->flush(); !flushed)
        {
            std::cerr << "Error flushing data: " << flushed.error() << std::endl;
        }
    }
}

std::expected<std::unique_ptr<CddContext>, ExpectedError> CddContext::create(const nlohmann::json& config) {
    try {
        const auto& backend_config = config.at("backend");
        const auto type = backend_config.at("type").get<std::string>();
        const auto mode_str = backend_config.at("mode").get<std::string>();

        std::unique_ptr<DataReader> reader;
        std::unique_ptr<DataWriter> writer;

        if (mode_str == "Read") {
            if (type != "File") return std::unexpected(ExpectedError("Read mode currently only supports File backend."));
            const auto path = backend_config.at("path").get<std::string>();
            auto reader_result = DataReader::open(path);
            if (!reader_result) return std::unexpected(ExpectedError(reader_result.error()));
            reader = std::move(*reader_result);
        } else { // WriteAppend or WriteTruncate
            size_t capacity = 1024;
            std::vector<std::byte> user_metadata; // Base64 parsing to be added later
             if (config.contains("writer_options")) {
                const auto& writer_opts = config.at("writer_options");
                capacity = writer_opts.value("chunk_offsets_block_capacity", 1024);
             }

            std::expected<std::unique_ptr<DataWriter>, std::string> writer_result;
            if (type == "File") {
                const auto path = backend_config.at("path").get<std::string>();
                if (mode_str == "WriteAppend") {
                    writer_result = DataWriter::open_for_append(path);
                } else { // WriteTruncate
                    writer_result = DataWriter::create_new(path, capacity, user_metadata);
                }
            } else if (type == "Memory") {
                if (mode_str != "WriteTruncate") return std::unexpected(ExpectedError("Memory backend only supports WriteTruncate mode."));
                writer_result = DataWriter::create_in_memory(capacity, user_metadata);
            } else {
                 return std::unexpected(ExpectedError("Unsupported backend type for writing: " + type));
            }

            if(!writer_result) return std::unexpected(ExpectedError(writer_result.error()));
            writer = std::move(*writer_result);
        }
        
        return std::make_unique<CddContext>(ProtectedMarker{}, std::move(reader), std::move(writer));

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
    // Using a static dispatch map is more efficient and scalable than an if-else chain.
    using HandlerFactory = std::function<std::unique_ptr<IOperationHandler>()>;
    static const std::unordered_map<std::string, HandlerFactory> op_handlers = {
        {"StoreChunk",      []() { return std::make_unique<StoreChunkHandler>(); }},
        {"StoreArray",      []() { return std::make_unique<StoreArrayHandler>(); }},
        {"Inspect",         []() { return std::make_unique<InspectHandler>(); }},
        {"LoadChunks",      []() { return std::make_unique<LoadChunksHandler>(); }},
        {"GetUserMetadata", []() { return std::make_unique<GetUserMetadataHandler>(); }},
        {"SetUserMetadata", []() { return std::make_unique<SetUserMetadataHandler>(); }},
        {"Flush",           []() { return std::make_unique<FlushHandler>(); }}
    };

    try {
        const std::string op_type = op_request.at("op_type").get<std::string>();

        auto it = op_handlers.find(op_type);
        if (it == op_handlers.end()) {
            // Handle special cases like "Ping" that don't need a handler class.
            if (op_type == "Ping") {
                return nlohmann::json{{"message", "Pong"}};
            }
            return std::unexpected(ExpectedError("Unknown or unsupported op_type: " + op_type));
        }

        // Create and execute the handler from the map.
        auto handler = it->second();
        return handler->execute(*this, op_request, input_data, output_data);

    } catch(const nlohmann::json::exception& e) {
        return std::unexpected(ExpectedError(std::string("JSON request error: ") + e.what()));
    }
}

} // namespace cryptodd::ffi
