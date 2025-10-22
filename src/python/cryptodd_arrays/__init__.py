"""
Start
"""

from cryptodd_arrays.cryptodd_arrays_cpp import _CddFile, CddException
import numpy as np

__version__ = "0.0.1"

def main():
    try:
        f = _CddFile(r"""{"backend": {"type": "Memory", "mode": "WriteTruncate"}}""")
    except CddException as e:
        print(f"{e.response_json}")
        raise
    f._execute_op(r"""
    {"operation": "ping"}
    """, input_data=None, output_data=None)

if __name__ == "__main__":
    main()