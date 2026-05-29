"""Reporting: sums fees across a batch of subtotals for a finance report.
A second DIRECT caller of fees.compute_fee, independent of the cart path."""

from fees import compute_fee


def total_fees(subtotals: list[int]) -> int:
    return sum(compute_fee(s) for s in subtotals)
