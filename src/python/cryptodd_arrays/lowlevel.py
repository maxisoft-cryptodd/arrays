# cryptodd_arrays/lowlevel.py
"""
A low-level wrapper around the compiled C++ extension.

This module isolates the C++/Python boundary from the rest of the library.
"""

import json
from typing import Any, Optional
import numpy as np

# This is the C++ binding. The name must match the PYBIND11_MODULE name.
from .cryptodd_arrays_cpp import _CddFile, CddException as CppCddException
from .exceptions import CddConfigError, CddOperationError

class LowLevelWrapper:
    """
    A thin, direct wrapper over the `_CddFile` C++ object.
    It handles JSON serialization/deserialization and exception translation.
    """
    def __init__(self, json_config: str):
        try:
            self._handle = _CddFile(json_config)
            self._closed = False
        except CppCddException as e:
            # Errors during creation are usually config-related
            raise CddConfigError.from_cpp_exception(e) from e
        except (RuntimeError, ValueError) as e:
            # Catch other potential pybind11 startup errors
            raise CddConfigError(f"Failed to create context: {e}") from e

    def execute(
        self,
        op_request: dict[str, Any],
        *,
        input_data: Optional[np.ndarray] = None,
        output_data: Optional[np.ndarray] = None
    ) -> dict[str, Any]:
        """
        Executes an operation via the C++ backend.

        Args:
            op_request: A dictionary representing the JSON request.
            input_data: An optional NumPy array for write operations.
            output_data: An optional, pre-allocated NumPy array for read ops.

        Returns:
            The 'result' dictionary from the C-API's JSON response.
        """
        if self._closed:
            raise ValueError("Operation attempted on a closed CddFile.")

        try:
            # Using compact separators is slightly more efficient
            json_op_str = json.dumps(op_request, separators=(',', ':'))
            response_bytes = self._handle._execute_op(
                json_op_str, input_data, output_data
            )
            response = json.loads(response_bytes)
            return response.get("result", {})
        except CppCddException as e:
            raise CddOperationError.from_cpp_exception(e) from e
        except (json.JSONDecodeError, TypeError) as e:
            # Should not happen with our builders, but good to have
            raise CddOperationError(
                f"Internal error during JSON processing: {e}",
                code=-1, code_message="Unknown Error", response_json={}
            ) from e

    def close(self) -> None:
        if not self._closed:
            self._handle.close()
            self._closed = True

    @property
    def closed(self) -> bool:
        return self._closed
