"""
Start
"""

from cryptodd_arrays.cryptodd_arrays_cpp import _CddFile, CddException

__version__ = "0.0.1"

def main():
    try:
        _CddFile(r"""{"backend": {"type": "Memory", "mode": "WriteTruncate"}}""")
    except CddException as e:
        print(f"{e.response_json}")

if __name__ == "__main__":
    main()