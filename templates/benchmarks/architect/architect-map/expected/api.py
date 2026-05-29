"""HTTP API layer: the entry point. Receives requests and delegates to the
service layer. Does no business logic or storage itself."""

from service import OrderService
from models import OrderRequest


def handle_create_order(payload: dict) -> dict:
    req = OrderRequest(item=payload["item"], qty=int(payload["qty"]))
    service = OrderService()
    order = service.place_order(req)
    return {"order_id": order.id, "status": order.status}
