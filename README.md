# Token Bucket Rate Limiter

A high-performance, thread-safe rate limiting service built in C++ using the Token Bucket algorithm.

## Overview

This project implements a rate limiter that controls how many requests a client can make within a given period.

Each client gets a token bucket:

- Requests consume tokens.
- Tokens are refilled at a fixed rate.
- Requests are allowed when tokens are available.
- Requests are rejected with HTTP `429` when the bucket is empty.

## Example

Configuration:

```text
Bucket Capacity : 10 tokens
Refill Rate     : 2 tokens/second