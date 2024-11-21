#ifndef SCTP_CLIENT_H
#define SCTP_CLIENT_H

//#include <netinet/sctp.h>
#include <functional>
#include <thread>
#include <string_view>
//#include <atomic>
//#include <string>

class SctpClient {
public:
    SctpClient( std::function<void(const std::string&&)> receiveCb,
                std::function<void(void)> connectionLostCb = nullptr,
                std::function<void(const std::string&&)> sendFailCb = nullptr,
                std::function<void(void)> peerChangeCb = nullptr );
    
    ~SctpClient();

    // Connect result enum
    enum class ConnectionResult : int_fast8_t {
        Success = 0,
        SocketError = -1,
        PeerConnectError = -2
    };

    [[nodiscard]] std::pair<ConnectionResult, std::string_view> connect(const std::string& address, int port);
    void close(void);
    bool send(const std::string& message);
    bool sendWithContext(const std::string& message, uint32_t context);

private:
    int _sockFd;
    volatile bool _isConnected;
    bool _isOpen;
    std::thread _receiveThread;

    // Callback functions
    std::function<void(const std::string&&)> _receiveCb;
    std::function<void(void)> _connectionLostCb;
    std::function<void(const std::string&&)> _sendFailCb;
    std::function<void(void)> _peerChangeCb;

    void _receiveLoop(void);
    void _handleNotification(const union sctp_notification*, size_t);
};

#endif // SCTP_CLIENT_H
