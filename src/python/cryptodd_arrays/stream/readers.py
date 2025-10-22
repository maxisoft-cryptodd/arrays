# cryptodd_arrays/stream/readers.py
"""
Advanced, high-level reader classes for streaming and grouped data.
"""
from typing import Dict, List, Sequence, Union, overload, Iterator
import numpy as np

from ..file import Reader
from ..dataclasses import ChunkInfo

class GroupedReader:
    """
    Reads synchronized, time-aligned arrays as a single, iterable stream.

    This reader is the counterpart to `GroupedWriter`. It discovers the
    interleaved chunk structure of multiple arrays and allows you to iterate
    over them group by group, where each group is a dictionary of arrays
    (e.g., `{"prices": prices_chunk_N, "volumes": volumes_chunk_N}`).

    It assumes that the arrays were written sequentially in repeating groups,
    for example: `p0, v0, p1, v1, p2, v2, ...`.

    Usage:
        with cdd.open("market_data.cdd", "r") as f:
            # Iterate over all chunk groups
            grouped_reader = GroupedReader(f, names=["prices", "volumes"])
            for chunk_group in grouped_reader:
                process(chunk_group["prices"], chunk_group["volumes"])

            # Access a specific chunk group by index
            third_group = grouped_reader[2]
    """
    def __init__(self, reader: Reader, names: Sequence[str]):
        """
        Initializes the grouped reader.

        Args:
            reader: An opened `cryptodd_arrays.Reader` instance.
            names: An ordered sequence of array names that constitute the group.
                   The order must match the order they were written in each cycle
                   (e.g., if written as p0, v0, p1, v1, then names must be
                   `["prices", "volumes"]`).
        """
        if not isinstance(reader, Reader):
            raise TypeError("reader must be a cryptodd_arrays.Reader object.")
        if not names:
            raise ValueError("`names` sequence cannot be empty.")

        self.reader = reader
        self.names = list(names)

        self._grouped_chunk_info: Dict[str, List[ChunkInfo]] = {name: [] for name in self.names}
        self._discover_and_validate_structure()

    def _discover_and_validate_structure(self):
        """Scans the file's chunks to build the group map and validate alignment."""
        all_chunks = self.reader.chunks
        num_names = len(self.names)

        if len(all_chunks) % num_names != 0:
            raise ValueError(
                f"File contains {len(all_chunks)} total chunks, which is not a "
                f"multiple of the number of grouped names ({num_names}). "
                "The file may be corrupt or not written by a GroupedWriter."
            )

        for i, chunk_info in enumerate(all_chunks):
            group_name = self.names[i % num_names]
            self._grouped_chunk_info[group_name].append(chunk_info)

        first_key = self.names[0]
        self._num_groups = len(self._grouped_chunk_info[first_key])

        for name in self.names[1:]:
            count = len(self._grouped_chunk_info[name])
            if count != self._num_groups:
                raise ValueError(
                    "Chunk count mismatch in grouped arrays. "
                    f"'{first_key}' has {self._num_groups} chunks, but "
                    f"'{name}' has {count}. The file is not properly grouped."
                )

    @property
    def num_groups(self) -> int:
        """The number of synchronized chunk groups in the file."""
        return self._num_groups

    def __len__(self) -> int:
        return self.num_groups

    @overload
    def __getitem__(self, key: int) -> Dict[str, np.ndarray]: ...

    @overload
    def __getitem__(self, key: slice) -> Iterator[Dict[str, np.ndarray]]: ...

    def __getitem__(self, key: Union[int, slice]) -> Union[Dict[str, np.ndarray], Iterator[Dict[str, np.ndarray]]]:
        """
        Retrieves a single chunk group or an iterator over a slice of groups.
        """
        if isinstance(key, int):
            if key < 0:
                key += self.num_groups
            if not (0 <= key < self.num_groups):
                raise IndexError("Group index out of range.")
            
            group_data: Dict[str, np.ndarray] = {}
            for name in self.names:
                physical_chunk_info = self._grouped_chunk_info[name][key]
                group_data[name] = self.reader[physical_chunk_info.index]
            return group_data
            
        elif isinstance(key, slice):
            start, stop, step = key.indices(self.num_groups)
            return (self[i] for i in range(start, stop, step))
            
        else:
            raise TypeError(f"Index must be an integer or slice, not {type(key).__name__}")

    def read_group(self, group_index: int) -> Dict[str, np.ndarray]:
        """Utility method to read a single, specific group of chunks."""
        return self[group_index]

    def __iter__(self) -> Iterator[Dict[str, np.ndarray]]:
        """
        Returns a new, independent iterator over the chunk groups.

        This implementation uses a generator, which is the standard Pythonic way
        to create iterators. It ensures that multiple `for` loops or calls to
        `iter()` on the same GroupedReader object will work correctly and
        independently.
        """
        for i in range(self.num_groups):
            yield self[i]

    def close(self) -> None:
        """Closes the underlying reader."""
        self.reader.close()

    def __enter__(self) -> "GroupedReader":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()