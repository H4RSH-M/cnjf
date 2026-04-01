# Game Networking Engine

A custom UDP-based multiplayer game networking engine built in C++. This project demonstrates a secure, server-authoritative architecture with continuous state broadcasting, lag simulation, and a real-time 2D graphical client.

## Core Features

* **Secure UDP:** Implements DTLS (Datagram Transport Layer Security) using OpenSSL to encrypt all packet traffic.
* **Server-Authoritative:** The server validates all client movements and broadcasts the definitive global state.
* **Continuous Heartbeat:** Clients send packets continuously (movement or silent heartbeats) to prevent connection timeouts.
* **Network Simulation:** Built-in lag and packet loss simulator to test engine resilience.
* **Cross-Platform:** Server runs natively on Linux; Client runs on Linux and Windows via MinGW.
* **Real-time GUI:** Frontend rendered at 60 FPS using Raylib.

## Project Structure

```text
/
├── CMakeLists.txt        # Cross-platform build configuration
├── server.crt            # DTLS Certificate (Must be generated)
├── server.key            # DTLS Private Key (Must be generated)
├── include/
│   └── protocol.h        # Shared packet definitions and structs
├── src/
│   ├── server_main.cpp   # Authoritative server logic
│   └── client_main.cpp   # Raylib frontend and client networking
