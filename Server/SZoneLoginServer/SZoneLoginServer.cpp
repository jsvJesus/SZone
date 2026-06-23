#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

static std::mutex GLogMutex;
static std::atomic_bool GRunning = true;

static std::string HexDump(const unsigned char* data, int size)
{
    std::ostringstream out;

    for (int i = 0; i < size; i += 16)
    {
        out << std::setw(8) << std::setfill('0') << std::hex << i << "  ";

        for (int j = 0; j < 16; ++j)
        {
            if (i + j < size)
            {
                out << std::setw(2) << std::setfill('0') << std::hex
                    << static_cast<int>(data[i + j]) << " ";
            }
            else
            {
                out << "   ";
            }
        }

        out << " ";

        for (int j = 0; j < 16; ++j)
        {
            if (i + j < size)
            {
                unsigned char c = data[i + j];
                out << ((c >= 32 && c <= 126) ? static_cast<char>(c) : '.');
            }
        }

        out << "\n";
    }

    return out.str();
}

static void SaveBinaryPacket(const char* fileName, const unsigned char* data, int size)
{
    std::ofstream file(fileName, std::ios::binary | std::ios::app);
    if (!file)
        return;

    uint32_t packetSize = static_cast<uint32_t>(size);
    file.write(reinterpret_cast<const char*>(&packetSize), sizeof(packetSize));
    file.write(reinterpret_cast<const char*>(data), size);
}

static void LogPacket(const std::string& source, const unsigned char* data, int size)
{
    std::lock_guard<std::mutex> lock(GLogMutex);

    std::cout << "\n==============================\n";
    std::cout << source << "\n";
    std::cout << "Size: " << std::dec << size << " bytes\n";
    std::cout << "==============================\n";
    std::cout << HexDump(data, size);
    std::cout << std::flush;
}

static void RunTcpServer(unsigned short port)
{
    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET)
    {
        std::cerr << "[TCP] socket failed: " << WSAGetLastError() << "\n";
        return;
    }

    BOOL reuse = TRUE;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "[TCP] bind failed: " << WSAGetLastError() << "\n";
        closesocket(listenSocket);
        return;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        std::cerr << "[TCP] listen failed: " << WSAGetLastError() << "\n";
        closesocket(listenSocket);
        return;
    }

    std::cout << "[TCP] Listening on 0.0.0.0:" << port << "\n";

    while (GRunning)
    {
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);

        SOCKET clientSocket = accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientSocket == INVALID_SOCKET)
        {
            if (GRunning)
                std::cerr << "[TCP] accept failed: " << WSAGetLastError() << "\n";
            continue;
        }

        char ip[64]{};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ip, sizeof(ip));

        {
            std::lock_guard<std::mutex> lock(GLogMutex);
            std::cout << "[TCP] Client connected: " << ip << ":" << ntohs(clientAddr.sin_port) << "\n";
        }

        std::thread([clientSocket, ip]()
        {
            std::vector<unsigned char> buffer(8192);

            while (GRunning)
            {
                int received = recv(clientSocket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);

                if (received > 0)
                {
                    std::string source = std::string("[TCP] Packet from ") + ip;
                    LogPacket(source, buffer.data(), received);
                    SaveBinaryPacket("tcp_packets.bin", buffer.data(), received);
                }
                else if (received == 0)
                {
                    std::lock_guard<std::mutex> lock(GLogMutex);
                    std::cout << "[TCP] Client disconnected: " << ip << "\n";
                    break;
                }
                else
                {
                    std::lock_guard<std::mutex> lock(GLogMutex);
                    std::cout << "[TCP] recv error: " << WSAGetLastError() << "\n";
                    break;
                }
            }

            closesocket(clientSocket);
        }).detach();
    }

    closesocket(listenSocket);
}

static void RunUdpServer(unsigned short port)
{
    SOCKET udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET)
    {
        std::cerr << "[UDP] socket failed: " << WSAGetLastError() << "\n";
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(udpSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "[UDP] bind failed: " << WSAGetLastError() << "\n";
        closesocket(udpSocket);
        return;
    }

    std::cout << "[UDP] Listening on 0.0.0.0:" << port << "\n";

    std::vector<unsigned char> buffer(8192);

    while (GRunning)
    {
        sockaddr_in fromAddr{};
        int fromLen = sizeof(fromAddr);

        int received = recvfrom(
            udpSocket,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&fromAddr),
            &fromLen
        );

        if (received > 0)
        {
            char ip[64]{};
            inet_ntop(AF_INET, &fromAddr.sin_addr, ip, sizeof(ip));

            std::ostringstream source;
            source << "[UDP] Packet from " << ip << ":" << ntohs(fromAddr.sin_port);

            LogPacket(source.str(), buffer.data(), received);
            SaveBinaryPacket("udp_packets.bin", buffer.data(), received);
        }
        else
        {
            std::cerr << "[UDP] recvfrom error: " << WSAGetLastError() << "\n";
        }
    }

    closesocket(udpSocket);
}

int main()
{
    constexpr unsigned short LoginPort = 22231;

    WSADATA wsaData{};
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);

    if (result != 0)
    {
        std::cerr << "WSAStartup failed: " << result << "\n";
        return 1;
    }

    std::cout << "SZone Login Packet Listener\n";
    std::cout << "Port: " << LoginPort << "\n";
    std::cout << "Run client after this server is started.\n";
    std::cout << "Press ENTER to stop.\n\n";

    std::thread tcpThread(RunTcpServer, LoginPort);
    std::thread udpThread(RunUdpServer, LoginPort);

    std::cin.get();

    GRunning = false;

    WSACleanup();

    std::cout << "Server stopped.\n";
    return 0;
}