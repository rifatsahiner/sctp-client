
#ifndef SCTP_CLIENT_H
#define SCTP_CLIENT_H

#include <functional>
#include <thread>
#include <variant>
#include <string_view>

class SctpClient {
public:
    SctpClient( std::variant< std::function<void(const std::string&&)>,                         // data only receive
                              std::function<void(const std::string&&, const uint_fast32_t)>     // data + context receive
                            > receiveCb,
                std::function<void(void)> connectionLostCb = nullptr,
                std::variant< std::function<void(const uint_fast32_t)>,                         // context only send failure
                              std::function<void(const std::string&&)>,                         // data only send failure
                              std::function<void(const std::string&&, const uint_fast32_t)>     // data + context send failure
                            > sendFailCb = (std::function<void(const uint_fast32_t)>) nullptr,
                uint_fast16_t sendTimeout = 10000);
    ~SctpClient();

    // sctp objects cannot get copied
    SctpClient(const SctpClient&) = delete;
    SctpClient& operator=(const SctpClient&) = delete;
    SctpClient(SctpClient&&) = delete;
    SctpClient& operator=(SctpClient&&) = delete;

    // Connect result enum
    enum class ConnectionResult : int_fast8_t {
        Success = 0,
        SocketError = -1,
        PeerConnectError = -2
    };

    // todo: method descriptions
    [[nodiscard]] std::pair<ConnectionResult, std::string_view> connect(const std::string& address, uint16_t port);
    void close(void);       // does not trigger connectionLostCb
    bool send(const std::string& message, uint_fast32_t context = UINT_FAST32_MAX);
    // bool send(const char* msgPtr, uint_fast16_t msgLen, uint_fast32_t context = UINT_FAST32_MAX);    // todo: eklenecek
    bool addPath(const std::string& address);       // !!! todo: not tested - server tarafinda farklı olarak once bind cagirilmasi gerekiyor
    bool removePath(const std::string& address);    // !!! not tested

private:
    int _sockFd;
    uint_fast16_t _timetoLive;
    volatile bool _isConnected;
    bool _isOpen;
    bool _closeTrigger;
    std::thread _receiveThread;

    uint_fast32_t _rcvErrCounter{0};
    uint_fast32_t _unhandledAssocChgCounter{0};
    uint_fast32_t _zeroLenMsgCounter{0};
    uint_fast32_t _notifSizeErrCounter{0};

    // Callback functions
    std::variant<std::function<void(const std::string&&)>, std::function<void(const std::string&&, const uint_fast32_t)>> _receiveCb;
    std::function<void(void)> _connectionLostCb;
    std::variant<std::function<void(const uint_fast32_t)>, std::function<void(const std::string&&)>, std::function<void(const std::string&&, const uint_fast32_t)>> _sendFailCb;

    void _receiveLoop(void);
    void _handleNotification(const union sctp_notification*, size_t);
};

#endif // SCTP_CLIENT_H
