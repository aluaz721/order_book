from app.models import OrderRequest
from app.services import EngineBridge


def test_submit_order_works_without_running_event_loop() -> None:
    bridge = EngineBridge()
    req = OrderRequest(side="buy", price=100.0, quantity=2, order_type="limit", symbol="AAPL")

    response = bridge.submit_order(req)

    assert response.accepted is True
    assert response.order_id == 1
    assert response.status == "accepted"
