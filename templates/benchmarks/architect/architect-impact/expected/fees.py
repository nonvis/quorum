"""Fee computation. The unit under change: compute_fee currently returns a
flat int (the fee amount in minor units)."""


def compute_fee(subtotal: int) -> int:
    # 2.5% fee, integer minor units
    return (subtotal * 25) // 1000
