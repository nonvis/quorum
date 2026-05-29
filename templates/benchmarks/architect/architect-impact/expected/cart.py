"""Cart: computes a line total including the fee. Direct caller of
fees.compute_fee."""

from fees import compute_fee


def cart_total(subtotal: int) -> int:
    fee = compute_fee(subtotal)
    return subtotal + fee
