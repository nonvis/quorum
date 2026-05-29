"""Shared data models. Imported by api, service, and repository. Depends on
nothing else — a leaf shared module."""

from dataclasses import dataclass


@dataclass
class OrderRequest:
    item: str
    qty: int


@dataclass
class Order:
    id: int
    item: str
    qty: int
    status: str
