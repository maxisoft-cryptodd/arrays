#include "cdd_context.h"
#include "operation_handler.h"
#include "store_chunk_handler.h"
#include "../data_io/data_reader.h"
#include "../data_io/data_writer.h"
#include "../storage/file_backend.h"
#include "../storage/memory_backend.h"
#include <string>
#include <iostream>

namespace cryptodd::ffi {

// Constructor now also initializes the compressor
CddContext::CddContext(
    ProtectedMarker,
    std::unique_ptr<DataReader>&& reader,
    std::unique_ptr<DataWriter>&& writer
) : reader_(std::move(reader)), writer_(std::move(writer)), compressor_() {}

CddContext::~CddContext() {
    if (writer_) {
        // Ensure data is flushed on context destruction
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
} // Correct closing brace for CddContext::create

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

std::expected<nlohmann::json, ExpectedError> CddContext::execute_operation(
    const nlohmann::json& op_request,
    std::span<const std::byte> input_data,
    std::span<const std::byte> output_data)
{
    try {
        const std::string op_type = op_request.at("op_type").get<std::string>();

        std::unique_ptr<IOperationHandler> handler;
        if (op_type == "Ping") {
            return nlohmann::json{{"message", "Pong"}};
        } else if (op_type == "StoreChunk") {
            handler = std::make_unique<StoreChunkHandler>();
        } else if (op_type == "StoreArray") {

            return std::unexpected(ExpectedError("op_type 'StoreArray' is not yet implemented."));
        } else {
            return std::unexpected(ExpectedError("Unknown or unsupported op_type: " + op_type));
        }

        return handler->execute(*this, op_request, input_data, output_data);

    } catch(const nlohmann::json::exception& e) {
        return std::unexpected(ExpectedError(std::string("JSON request error: ") + e.what()));
    }
}

} // namespace cryptodd::ffi
