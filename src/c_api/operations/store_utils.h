#pragma once

#include "../operations/operation_types.h"
#include "../cdd_context.h"
#include <expected>
#include <span>

namespace cryptodd { class DataWriter; }

namespace cryptodd::ffi::StoreUtils {

std::expected<ChunkWriteDetails, ExpectedError> compress_and_write_chunk(
    CddContext& context, cryptodd::DataWriter& writer, const DataSpec& data_spec,
    const EncodingSpec& encoding_spec, std::span<const std::byte> chunk_input_data);

} // namespace cryptodd::ffi::StoreUtils
