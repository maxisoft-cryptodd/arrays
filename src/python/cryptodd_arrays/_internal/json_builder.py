# cryptodd_arrays/_internal/json_builder.py

"""
Internal functions to build the JSON request dictionaries for the C-API.

This module decouples the high-level API from the specific JSON format
required by the C++ backend, making future C-API changes easier to manage.
"""

import json
import base64
from typing import Any, Optional, TypeAlias
import numpy as np

from ..types import Codec
from . import numpy_utils

# TypeAlias for clarity in function signatures.
JsonRequest: TypeAlias = dict[str, Any]

def _normalize_codec(codec: Codec | str) -> str:
    """Converts a Codec enum or string to the required C-API string name."""
    return codec.name if isinstance(codec, Codec) else str(codec).upper()

def build_store_chunk_req(
    arr: np.ndarray,
    codec: Codec | str,
    codec_params: dict[str, Any]
) -> JsonRequest:
    """Builds the JSON request for the 'StoreChunk' operation."""
    encoding_spec = {
        "codec": _normalize_codec(codec),
        **codec_params,
    }
    return {
        "op_type": "StoreChunk",
        "data_spec": numpy_utils.get_dataspec(arr),
        "encoding": encoding_spec,
    }

def build_load_chunks_req(
    selection_key: int | slice | None,
    check_checksums: bool
) -> JsonRequest:
    """
    Builds the JSON request for the 'LoadChunks' operation.

    Args:
        selection_key: An integer index, a resolved slice, or None (for all).
                       The caller (`Reader`) is responsible for resolving
                       negative indices and open-ended slices (e.g., `data[2:]`)
                       into a concrete slice with non-negative start/stop values.
        check_checksums: Whether to verify checksums during load.
    """
    selection_dict: dict[str, Any]
    if selection_key is None:
        selection_dict = {"type": "All"}
    elif isinstance(selection_key, int):
        # The C-API uses Range for single-item selection.
        # The caller is responsible for resolving negative indices.
        if selection_key < 0:
            raise IndexError("Negative indices must be resolved by the high-level caller.")
        selection_dict = {
            "type": "Range",
            "start_index": selection_key,
            "count": 1
        }
    elif isinstance(selection_key, slice):
        if selection_key.step is not None and selection_key.step != 1:
            raise IndexError("Slicing with a step is not supported by the backend.")

        # The caller MUST provide a resolved slice. start/stop cannot be None.
        if selection_key.start is None or selection_key.stop is None:
             raise ValueError("Slice start and stop must be resolved to concrete integers by the caller.")

        count = selection_key.stop - selection_key.start
        if count < 0:
            count = 0

        selection_dict = {
            "type": "Range",
            "start_index": selection_key.start,
            "count": count
        }
    else:
        raise TypeError(f"Unsupported selection key type: {type(selection_key)}")

    return {
        "op_type": "LoadChunks",
        "selection": selection_dict,
        "check_checksums": check_checksums,
    }

def build_set_user_metadata_req(metadata: dict) -> JsonRequest:
    """Builds the request to set user metadata."""
    try:
        # Pipeline: dict -> json str -> utf-8 bytes -> base64 bytes -> ascii str
        json_str = json.dumps(metadata, separators=(',', ':'))
        utf8_bytes = json_str.encode('utf-8')
        b64_bytes = base64.b64encode(utf8_bytes)
        b64_str = b64_bytes.decode('ascii')
    except TypeError as e:
        raise TypeError(f"Metadata must be JSON-serializable. {e}") from e

    return {
        "op_type": "SetUserMetadata",
        "user_metadata_base64": b64_str,
    }

def build_inspect_req() -> JsonRequest:
    """Builds the JSON request for the 'Inspect' operation."""
    return {"op_type": "Inspect"}

def build_get_user_metadata_req() -> JsonRequest:
    """Builds the JSON request for the 'GetUserMetadata' operation."""
    return {"op_type": "GetUserMetadata"}

def build_flush_req() -> JsonRequest:
    """Builds the JSON request for the 'Flush' operation."""
    return {"op_type": "Flush"}
