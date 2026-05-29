"""Catalog: lists products and their sticker prices. Touches no fee logic at
all — independent of the fees subsystem. The insulated component."""

_PRODUCTS = {
    "widget": 1000,
    "gadget": 2500,
}


def price_of(sku: str) -> int:
    return _PRODUCTS[sku]


def list_skus() -> list[str]:
    return list(_PRODUCTS.keys())
