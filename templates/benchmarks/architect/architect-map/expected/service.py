"""Service layer: business logic. Validates the request, builds an Order,
and hands it to the repository to persist. Knows nothing about HTTP or the
raw database driver."""

from repository import OrderRepository
from models import Order, OrderRequest


class OrderService:
    def __init__(self) -> None:
        self.repo = OrderRepository()

    def place_order(self, req: OrderRequest) -> Order:
        if req.qty <= 0:
            raise ValueError("qty must be positive")
        order = Order(id=0, item=req.item, qty=req.qty, status="pending")
        return self.repo.save(order)
