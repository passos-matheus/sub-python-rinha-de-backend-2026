package main

import (
	"net/http"
)

var fraudResponse = []byte(`{"approved":false,"fraud_score":1.0}`)

func ready(w http.ResponseWriter, r *http.Request) {
	w.WriteHeader(http.StatusOK)
}

func fraudScore(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.Write(fraudResponse)
}

func main() {
	mux := http.NewServeMux()
	mux.HandleFunc("/ready", ready)
	mux.HandleFunc("/fraud-score", fraudScore)

	server := &http.Server{
		Addr:    ":3000",
		Handler: mux,
	}

	println("Listening on port 3000")
	if err := server.ListenAndServe(); err != nil {
		panic(err)
	}
}