# cryptodd_arrays/abc.py
"""Abstract Base Classes for the cryptodd_arrays library."""

import abc

class CddFileBase(abc.ABC):
    """Abstract base class for Cryptodd Arrays file handlers."""

    @abc.abstractmethod
    def close(self) -> None:
        """
        Closes the file handle and flushes any pending writes.
        Subsequent operations on the object will raise an error.
        """
        raise NotImplementedError

    @property
    @abc.abstractmethod
    def closed(self) -> bool:
        """Returns True if the file handle is closed."""
        raise NotImplementedError

    def __enter__(self) -> "CddFileBase":
        if self.closed:
            raise ValueError("Cannot enter context with a closed file handle.")
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()
