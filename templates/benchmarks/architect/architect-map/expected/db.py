"""DB layer: the low-level connection + a trivial in-memory store. The sink
the repository writes through. Depends on nothing else in the project."""

_NEXT_ID = [0]
_TABLES: dict = {}


class Connection:
    def insert(self, table: str, row: dict) -> int:
        _NEXT_ID[0] += 1
        _TABLES.setdefault(table, {})[_NEXT_ID[0]] = row
        return _NEXT_ID[0]


def get_connection() -> Connection:
    return Connection()
