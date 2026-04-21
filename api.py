from socketify import App



app = App()



FRAUD_RESPONSE = b'{"approved":false,"fraud_score":1.0}'

def ready(res, req):
    res.end("")

def fraud_score(res, req):
    res.write_header("Content-Type", "application/json")
    res.end(FRAUD_RESPONSE)

app.get("/ready", ready)
app.post("/fraud-score", fraud_score)
app.listen(3000, lambda config: print("Listening on port 3000"))
app.run()
