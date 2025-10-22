# cryptodd_arrays/exceptions.py
"""Custom exception types for the cryptodd_arrays library."""

import json
from typing import Any, Optional

class CddError(Exception):
    """Base exception for all errors raised by this library."""
    pass

class CddConfigError(CddError):
    """Error related to configuration or setup, such as an invalid file path."""
    pass

class CddOperationError(CddError):
    """
    Error raised when a valid C-API operation fails during execution.

    This typically wraps an error returned from the C++ backend.

    Attributes:
        message (str): The primary error message.
        code (int): The C-API error code (e.g., -4 for OPERATION_FAILED).
        code_message (str): The string representation of the C-API error code.
        response_json (dict): The full JSON error response from the backend.
    """
    def __init__(
        self,
        message: str,
        *,
        code: int,
        code_message: str,
        response_json: dict[str, Any]
    ):
        super().__init__(message)
        if getattr(self, message, '') != message:
            self.message = message
        self.code = code
        self.code_message = code_message
        self.response_json = response_json

    def __str__(self) -> str:
        return f"{self.message} (code={self.code}, name='{self.code_message}')"

    @classmethod
    def from_cpp_exception(cls, cpp_exc: Exception) -> "CddOperationError":
        """Factory method to create a CddOperationError from a C++ CddException."""
        # The pybind11 exception has these attributes we defined
        response_json_str = getattr(cpp_exc, "response_json", "{}")
        code = getattr(cpp_exc, "code", -1) # -1 is CDD_ERROR_UNKNOWN
        code_message = getattr(cpp_exc, "code_message", "Unknown Error")

        try:
            response_data = json.loads(response_json_str)
            # Extract the most specific message from the JSON if available
            message = response_data.get("error", {}).get("message", str(cpp_exc))
        except (json.JSONDecodeError, AttributeError):
            response_data = {"raw_response": response_json_str}
            message = str(cpp_exc)

        return cls(
            message=message,
            code=code,
            code_message=code_message,
            response_json=response_data,
        )
