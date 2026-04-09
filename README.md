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

    /
    ├── CMakeLists.txt        # Cross-platform build configuration
    ├── server.crt            # DTLS Certificate (Must be generated)
    ├── server.key            # DTLS Private Key (Must be generated)
    ├── include/
    │   └── protocol.h        # Shared packet definitions and structs
    ├── src/
    │   ├── server_main.cpp   # Authoritative server logic
    │   └── client_main.cpp   # Raylib frontend and client networking

## Prerequisites and Setup

### For Linux (Ubuntu/Debian) Server & Client
Ensure you have standard build tools, OpenSSL, and the required graphics libraries for Raylib installed. Run the following in your terminal:

    sudo apt update
    sudo apt install build-essential cmake libssl-dev xorg-dev libgl1-mesa-dev

### For Windows Client (MinGW)
Building the C++ client on Windows requires specific routing to bypass library conflicts between GCC and Windows MSVC binaries.

1. **Install MSYS2:** Download from msys2.org and install to the default location.
2. **Install Compiler Toolchain:** Open the MSYS2 MinGW x64 terminal and run:
   
       pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make

3. **Install OpenSSL (Shining Light):** Download the Win64 OpenSSL v3.x.x EXE (Full, not Light). Install it directly to the root of your C drive: C:\OpenSSL-Win64. 
   *(Note: Avoid installing to "Program Files" to prevent MinGW pathing errors with spaces).*

## Configuration

### Generate SSL Certificates
The DTLS server requires a certificate and private key in the root directory. Run this command in the project root:

    openssl req -x509 -newkey rsa:2048 -nodes -keyout server.key -out server.crt -days 365

### Set Target IP
Open src/client_main.cpp. By default, the client looks for the server on localhost. If you are running the server on a VM or another machine, change the definition on line 49:

    #define SERVER_IP "127.0.0.1" // Change this to your Server's IP address

## Building

### On Linux
    mkdir build
    cd build
    cmake ..
    make

### On Windows
Open your command prompt and run:
    
    mkdir build
    cd build
    cmake -G "MinGW Makefiles" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ..
    mingw32-make

**Important Build Note for Windows:** In the CMakeLists.txt file, the paths to search for libssl.lib and libcrypto.lib are hardcoded. You must change these paths to match your exact OpenSSL installation site if they differ from the default script.

## Execution

### Starting the Server (Linux Only)
The server must be running before clients attempt to connect.
    
    cd build
    ./server

### Starting the Client

**Linux:**
    
    cd build
    ./client

**Windows:**
    
    cd build
    client.exe

## Controls & Gameplay

After the client successfully compiles and connects, the Raylib GUI window will open. 

* **Blue Circle:** Your local player.
* **Red Circles:** Other connected clients updating via the server broadcast.
* **Movement:** Use W A S D or the Arrow Keys to move around the screen.
