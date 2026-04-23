FRAUD_START = {
    "type": "http.response.start",
    "status": 200,
    "headers": [(b"content-type", b"application/json")],
}
FRAUD_BODY = {
    "type": "http.response.body",
    "body": b'{"approved":false,"fraud_score":1.0}',
}
READY_START = {"type": "http.response.start", "status": 200, "headers": []}
EMPTY_BODY = {"type": "http.response.body"}


async def app(scope, receive, send):
    if scope["type"] != "http":
        return
    if scope["path"] == "/fraud-score":
        await send(FRAUD_START)
        await send(FRAUD_BODY)
    else:
        await send(READY_START)
        await send(EMPTY_BODY)
