#include <winsock2.h>//winsock2
#include <ws2tcpip.h>//�׽���
#include <iostream>//���
#include <thread>//���߳�
#include <vector>//����
#include <fstream>//�ļ���д
#include <nlohmann/json.hpp> //��������
#include <filesystem> //�ļ�ϵͳ
#include "conlog.h" // ��־����

using json = nlohmann::json;
namespace fs = std::filesystem;

struct ForwardRule {
    std::string name;
    std::string listen;
    std::string target;
    std::string protocol;
};

SemaphoreQueue<std::string> logQueue;

void LogWorker() {
    while (true) {
        std::string logMessage = logQueue.Dequeue();
        std::cout << logMessage << std::endl;
    }
}

void Log(const std::string& message) {
    logQueue.Enqueue(message);
}

SOCKET CreateSocket(const addrinfo* info) {
    SOCKET sock = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
    if (sock == INVALID_SOCKET) {
        LogSocketError(WSAGetLastError());
        return INVALID_SOCKET;
    }

    // ���� SO_REUSEADDR ѡ��
    int optval = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval, sizeof(optval)) == SOCKET_ERROR) {
        LogSocketError(WSAGetLastError());
        closesocket(sock);
        return INVALID_SOCKET;
    }

    if (bind(sock, info->ai_addr, (int)info->ai_addrlen) == SOCKET_ERROR) {
        LogSocketError(WSAGetLastError());
        closesocket(sock);
        return INVALID_SOCKET;
    }

    return sock;
}

void ForwardTCP(SOCKET client, const sockaddr_storage& targetAddr) {
    SOCKET server = socket(targetAddr.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        LogSocketError(WSAGetLastError());
        closesocket(client);
        return;
    }

    if (connect(server, (sockaddr*)&targetAddr, sizeof(targetAddr)) == SOCKET_ERROR) {
        LogSocketError(WSAGetLastError());
        closesocket(client);
        closesocket(server);
        return;
    }

    auto forward = [](SOCKET from, SOCKET to) {
        char buffer[4096];
        while (true) {
            int len = recv(from, buffer, sizeof(buffer), 0);
            if (len <= 0) {
                if (len == 0) {
                    Log("Connection closed by peer.");
                }
                else {
                    LogSocketError(WSAGetLastError());
                }
                break;
            }
            if (send(to, buffer, len, 0) == SOCKET_ERROR) {
                LogSocketError(WSAGetLastError());
                break;
            }
        }
        closesocket(from);
        closesocket(to);
        };

    std::thread(forward, client, server).detach();
    std::thread(forward, server, client).detach();
}

void HandleUDP(SOCKET sock, const sockaddr_storage& targetAddr) {
    char buffer[4096];
    sockaddr_storage clientAddr;
    int addrLen = sizeof(clientAddr);

    while (true) {
        int len = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&clientAddr, &addrLen);
        if (len <= 0) {
            if (len == 0) {
                Log("Client disconnected.");
            }
            else {
                LogSocketError(WSAGetLastError());
            }
            break;
        }

        char clientIP[NI_MAXHOST];
        getnameinfo((sockaddr*)&clientAddr, addrLen, clientIP, sizeof(clientIP), nullptr, 0, NI_NUMERICHOST);
        Log("���յ����� " + std::string(clientIP) + " �� UDP ����");

        if (sendto(sock, buffer, len, 0, (sockaddr*)&targetAddr, sizeof(targetAddr)) == SOCKET_ERROR) {
            LogSocketError(WSAGetLastError());
            break;
        }

        // ����Ŀ���ַ����Ӧ��ת���ؿͻ���
        len = recvfrom(sock, buffer, sizeof(buffer), 0, nullptr, nullptr);
        if (len > 0) {
            if (sendto(sock, buffer, len, 0, (sockaddr*)&clientAddr, addrLen) == SOCKET_ERROR) {
                LogSocketError(WSAGetLastError());
                break;
            }
        }
    }
    closesocket(sock);
}

void StartForwarding(const ForwardRule& rule) {
    addrinfo hints{}, * listenInfo = nullptr, * targetInfo = nullptr;

    // ����������ַ�Ͷ˿�
    std::string listenAddress = rule.listen.substr(0, rule.listen.find(':'));
    std::string listenPort = rule.listen.substr(rule.listen.find(':') + 1);

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_socktype = (rule.protocol == "tcp") ? SOCK_STREAM : SOCK_DGRAM;

    Log("Resolving listen address: " + listenAddress + ":" + listenPort);
    if (getaddrinfo(listenAddress.c_str(), listenPort.c_str(), &hints, &listenInfo) != 0) {
        Log("Failed to get address info for listen address: " + listenAddress + ":" + listenPort + ". Error: " + std::to_string(WSAGetLastError()));
        return;
    }

    // ����Ŀ���ַ�Ͷ˿�
    std::string targetAddress = rule.target.substr(0, rule.target.find(':'));
    std::string targetPort = rule.target.substr(rule.target.find(':') + 1);

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_socktype = (rule.protocol == "tcp") ? SOCK_STREAM : SOCK_DGRAM;

    Log("Resolving target address: " + targetAddress + ":" + targetPort);
    if (getaddrinfo(targetAddress.c_str(), targetPort.c_str(), &hints, &targetInfo) != 0) {
        Log("Failed to get address info for target address: " + targetAddress + ":" + targetPort + ". Error: " + std::to_string(WSAGetLastError()));
        freeaddrinfo(listenInfo);
        return;
    }

    SOCKET listenSocket = CreateSocket(listenInfo);
    if (listenSocket == INVALID_SOCKET) {
        freeaddrinfo(listenInfo);
        freeaddrinfo(targetInfo);
        return;
    }

    if (rule.protocol == "tcp") {
        if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
            Log("Failed to listen on socket: " + std::to_string(WSAGetLastError()));
            closesocket(listenSocket);
            freeaddrinfo(listenInfo);
            freeaddrinfo(targetInfo);
            return;
        }

        std::thread([=] {
            while (true) {
                sockaddr_storage clientAddr;
                int addrLen = sizeof(clientAddr);
                SOCKET client = accept(listenSocket, (sockaddr*)&clientAddr, &addrLen);
                if (client == INVALID_SOCKET) {
                    Log("Failed to accept client connection: " + std::to_string(WSAGetLastError()));
                    continue;
                }
                char clientIP[NI_MAXHOST];
                getnameinfo((sockaddr*)&clientAddr, addrLen, clientIP, sizeof(clientIP), nullptr, 0, NI_NUMERICHOST);
                Log("���յ����� " + std::string(clientIP) + " �� TCP ����");

                if (targetInfo != nullptr && targetInfo->ai_addr != nullptr) {
                    std::thread(ForwardTCP, client, *(sockaddr_storage*)targetInfo->ai_addr).detach();
                }
                else {
                    Log("targetInfo �� targetInfo->ai_addr �ǿ�ָ��");
                }
            }
            closesocket(listenSocket);
            }).detach();
    }
    else {
        std::thread(HandleUDP, listenSocket, *(sockaddr_storage*)targetInfo->ai_addr).detach();
    }

    freeaddrinfo(listenInfo);
    freeaddrinfo(targetInfo);
}


void CreateDefaultConfig(const std::string& filePath) {
    json defaultConfig = {
        {"forward_rules", {
            {
                {"name", "example_rule"},
                {"listen", "127.0.0.1:5555"}, // Ĭ�ϼ�����ַ�Ͷ˿�
                {"target", "127.0.0.1:7777"}, // Ĭ��Ŀ���ַ�Ͷ˿�
                {"protocol", "tcp"} // Ĭ��Э��
            }
        }}
    };

    std::ofstream configFile(filePath);
    if (!configFile.is_open()) {
        Log("Failed to create default config file.");
        return;
    }
    configFile << defaultConfig.dump(4);
    configFile.close();
}

std::string GetExecutablePath() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::string::size_type pos = std::string(buffer).find_last_of("\\/");
    return std::string(buffer).substr(0, pos);
}

int main() {
    std::thread logThread(LogWorker);
    logThread.detach();

    // ��ȡ��ִ���ļ�����Ŀ¼
    std::string exePath = GetExecutablePath();
    std::cout << "Executable path: " << exePath << std::endl;

    // ���õ�ǰ����Ŀ¼Ϊ��ִ���ļ�����Ŀ¼
    fs::current_path(exePath);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Log("WSAStartup failed.");
        return 1;
    }

    std::string configFilePath = "config.json";
    if (!fs::exists(configFilePath)) {
        Log("Config file not found. Creating default config file.");
        CreateDefaultConfig(configFilePath);
    }

    std::ifstream configFile(configFilePath);
    if (!configFile.is_open()) {
        Log("Failed to open config file.");
        WSACleanup();
        return 1;
    }

    json config;
    try {
        config = json::parse(configFile);
    }
    catch (const json::parse_error& e) {
        Log("Failed to parse config file: " + std::string(e.what()));
        WSACleanup();
        return 1;
    }

    std::vector<ForwardRule> rules;
    for (auto& rule : config["forward_rules"]) {
        rules.push_back({
            rule["name"],
            rule["listen"],
            rule["target"],
            rule["protocol"]
            });
    }

    for (auto& rule : rules) {
        StartForwarding(rule);
    }

    Log("Port forwarder running. Press Enter to exit...");
    std::cin.get();

    WSACleanup();
    return 0;
}

