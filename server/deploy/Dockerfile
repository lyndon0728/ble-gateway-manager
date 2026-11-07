ARG GO_IMAGE=golang:1.27.3-alpine3.24
FROM ${GO_IMAGE} AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/gateway-server ./cmd/gateway-server \
 && CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/gatewayctl ./cmd/gatewayctl \
 && CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/gateway-simulator ./cmd/gateway-simulator \
 && CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/customer-simulator ./cmd/customer-simulator
FROM alpine:3.24
RUN apk add --no-cache ca-certificates tzdata \
 && addgroup -g 10001 gateway && adduser -D -H -u 10001 -G gateway gateway
COPY --from=build /out/ /usr/local/bin/
USER 10001:10001
WORKDIR /app
EXPOSE 8443
ENTRYPOINT ["gateway-server"]
