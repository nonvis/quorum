"""Repository layer: persistence. Translates Order objects to/from rows and
talks to the db connection module. The terminal sink of the write path."""

from db import get_connection
from models import Order


class OrderRepository:
    def save(self, order: Order) -> Order:
        conn = get_connection()
        order.id = conn.insert("orders", {
            "item": order.item,
            "qty": order.qty,
            "status": order.status,
        })
        return order
