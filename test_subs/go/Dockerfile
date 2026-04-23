FROM golang:1.23-alpine AS builder
WORKDIR /app
COPY api.go .
RUN go mod init api && \
    CGO_ENABLED=0 GOOS=linux go build -ldflags="-s -w" -o api api.go

FROM scratch
COPY --from=builder /app/api /api
EXPOSE 3000
ENTRYPOINT ["/api"]