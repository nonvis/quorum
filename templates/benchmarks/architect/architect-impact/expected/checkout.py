"""Checkout: finalizes an order using the cart total. Does NOT call
compute_fee directly — it depends on cart, which depends on fees."""

from cart import cart_total


def finalize(subtotal: int) -> dict:
    total = cart_total(subtotal)
    return {"total": total, "status": "paid"}
