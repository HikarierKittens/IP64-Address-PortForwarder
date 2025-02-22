#include <winsock2.h>//winsock2
#include <ws2tcpip.h>//套接字
#include <iostream>//输出
#include <thread>//多线程
#include <vector>//队列
#include <fstream>//文件读写
#include <nlohmann/json.hpp> //第三方库
#include <filesystem> //文件系统
#include "conlog.h" // 日志处理

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
    // 输出调试信息
//    std::cerr << "Debug: Logging message: " << message << std::endl;

    logQueue.Enqueue(message);
}

SOCKET CreateSocket(const addrinfo* info) {
    SOCKET sock = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
    if (sock == INVALID_SOCKET) {
        LogSocketError(WSAGetLastError());
        return INVALID_SOCKET;
    }

    // 设置 SO_REUSEADDR 选项
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

void HandleUDP(SOCKET sock, const sockaddr_storage& targetAddr) { //UDP转发
    char buffer[4096];
    sockaddr_storage clientAddr;
    int addrLen = sizeof(clientAddr);

    while (true) {
        // 重置 addrLen
        addrLen = sizeof(clientAddr);

        // 接收来自客户端的数据
        int len = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&clientAddr, &addrLen);
        if (len <= 0) {
            if (len == 0) {
                Log("Client disconnected.");
            }
            else {
                LogSocketError(WSAGetLastError());
            }
            continue;
        }

        char clientIP[NI_MAXHOST];
        getnameinfo((sockaddr*)&clientAddr, addrLen, clientIP, sizeof(clientIP), nullptr, 0, NI_NUMERICHOST);
        Log("接收到来自 " + std::string(clientIP) + " 的 UDP 连接");

        // 将数据转发到目标地址
        if (sendto(sock, buffer, len, 0, (sockaddr*)&targetAddr, sizeof(targetAddr)) == SOCKET_ERROR) {
            LogSocketError(WSAGetLastError());
            continue;
        }

        // 重置 targetAddrLen
        int targetAddrLen = sizeof(targetAddr);

        // 接收目标地址的响应并转发回客户端
        sockaddr_storage targetResponseAddr;
        len = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&targetResponseAddr, &targetAddrLen);
        if (len > 0) {
            if (sendto(sock, buffer, len, 0, (sockaddr*)&clientAddr, addrLen) == SOCKET_ERROR) {
                LogSocketError(WSAGetLastError());
                continue;
            }
        }
    }
    closesocket(sock);
}

void StartForwarding(const ForwardRule& rule) { //启动转发步骤，开始解析地址和端口，创建套接字，启动线程
    addrinfo hints{}, * listenInfo = nullptr, * targetInfo = nullptr;//解析地址和端口的结构体，listen存储监听地址和端口，target存储目标地址和端口,初始化为nullptr

    // 解析监听地址和端口
    std::string listen_Address, listen_Port;
    SeparateIpAndPort_listen(rule.listen, listen_Address, listen_Port);

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_UNSPEC; // 支持 IPv4 和 IPv6
    hints.ai_socktype = (rule.protocol == "tcp") ? SOCK_STREAM : SOCK_DGRAM;

    Log("Resolving listen address: " + listen_Address + ":" + listen_Port);
    if (getaddrinfo(listen_Address.c_str(), listen_Port.c_str(), &hints, &listenInfo) != 0) {
        LogSocketError(WSAGetLastError());
        return;
    }

    // 解析目标地址和端口
    std::string target_Address, target_Port;
    SeparateIpAndPort_target(rule.target, target_Address, target_Port);

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // 支持 IPv4 和 IPv6
    hints.ai_socktype = (rule.protocol == "tcp") ? SOCK_STREAM : SOCK_DGRAM;

    Log("Resolving target address: " + target_Address + ":" + target_Port);
    if (getaddrinfo(target_Address.c_str(), target_Port.c_str(), &hints, &targetInfo) != 0) {
        LogSocketError(WSAGetLastError());
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
                Log("接收到来自 " + std::string(clientIP) + " 的 TCP 连接");

                if (targetInfo != nullptr && targetInfo->ai_addr != nullptr) {
                    std::thread(ForwardTCP, client, *(sockaddr_storage*)targetInfo->ai_addr).detach();
                }
                else {
                    Log("targetInfo 或 targetInfo->ai_addr 是空指针");
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

void CreateDefaultConfig(const std::string& filePath) {  //创建默认配置文件
    json defaultConfig = {
        {"forward_rules", {
            {
                {"name", "example1_rule"},
                {"listen", "127.0.0.1:19555"}, // 监听地址和端口
                {"target", "127.0.0.1:19666"}, // 目标地址和端口
                {"protocol", "tcp"} // 协议类型
            },
            {
                {"name", "example2_rule"},
                {"listen", "[::1]:19777"}, // 监听地址和端口_IPv6
                {"target", "[::1]:19888"}, // 目标地址和端口_IPv6
                {"protocol", "tcp"} // 协议类型
            },
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
    system("chcp 65001");//设置UTF-8编码

    // 启动日志线程并分离
    std::thread logThread(LogWorker);
    logThread.detach();

    // 获取可执行文件所在目录
    std::string exePath = GetExecutablePath();

    // 设置当前工作目录为可执行文件所在目录
    fs::current_path(exePath);

    // 初始化Winsock库
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Log("WSAStartup failed.");
        return 1;
    }

    // 检查配置文件是否存在，如果不存在则创建默认配置文件
    std::string configFilePath = "config.json";
    if (!fs::exists(configFilePath)) {
        Log("Config file not found. Creating default config file.");
        CreateDefaultConfig(configFilePath);
    }

    // 打开配置文件
    std::ifstream configFile(configFilePath);
    if (!configFile.is_open()) {
        Log("Failed to open config file.");
        WSACleanup();
        return 1;
    }

    // 解析配置文件内容到json对象
    json config;
    try {
        config = json::parse(configFile);
    }
    catch (const json::parse_error& e) {
        Log("Failed to parse config file: " + std::string(e.what()));
        WSACleanup();
        return 1;
    }

    // 从配置文件中读取转发规则并存储到规则列表中
    std::vector<ForwardRule> rules;
    if (config.contains("forward_rules") && config["forward_rules"].is_array()) {
        for (const auto& rule : config["forward_rules"]) {
            if (rule.contains("name") && rule.contains("listen") && rule.contains("target") && rule.contains("protocol")) {
                rules.push_back({
                    rule["name"].get<std::string>(),
                    rule["listen"].get<std::string>(),
                    rule["target"].get<std::string>(),
                    rule["protocol"].get<std::string>()
                    });
            }
            else {
                Log("Invalid rule format in config file--配置文件读取错误.");
            }
        }
    }
    else {
        Log("No valid forward_rules found in config file.");
    }

    // 根据规则列表启动端口转发
    for (const auto& rule : rules) {
        StartForwarding(rule);
    }

    Log("Port forwarder running. Press Enter to exit...");
    std::cin.get();

    // 清理Winsock库
    WSACleanup();
    return 0;
}
