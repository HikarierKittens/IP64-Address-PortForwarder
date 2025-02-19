#include <string>
#include <winsock2.h>
#include "conlog.h"
#include <iostream>
#include <thread> // �����߳̿�
#include <chrono> // ����chrono�⣬���ڴ���ʱ��




void LogSocketError(int errorCode) {
    switch (errorCode) {
    case WSAECONNABORTED:
        Log("�����ѱ�һ����ֹ (WSAECONNABORTED, 10053)");
        break;
    case WSAECONNRESET:
        Log("���ӱ��Է����� (WSAECONNRESET, 10054)");
        break;
    case WSAEHOSTUNREACH:
        Log("Ŀ�������޷����� (WSAEHOSTUNREACH, 10065)");
        break;
    case WSAENETDOWN:
        Log("������ϵͳ������ (WSAENETDOWN, 10050)");
        break;
    case WSAENETRESET:
        Log("���������ѱ����� (WSAENETRESET, 10052)");
        break;
    case WSAENETUNREACH:
        Log("���粻�ɴ� (WSAENETUNREACH, 10051)");
        break;
    case WSAETIMEDOUT:
        Log("���ӳ�ʱ (WSAETIMEDOUT, 10060)");
        break;
    case WSATYPE_NOT_FOUND:
        Log("δ֪�ĵ�ַ���� (WSATYPE_NOT_FOUND, 10109)");
        Log("���������ļ��Ƿ���ȷ");
        break;
        // ������������Ӹ���Ĵ�����
    default:
        Log(("����δ֪���󣬴������: " + std::to_string(errorCode)).c_str());
        break;
    }
}
