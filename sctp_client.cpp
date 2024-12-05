
#include "sctp_client.h"

#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/sctp.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <iostream>

#define RECV_BUFFER_SIZE 2048

SctpClient::SctpClient( std::variant<std::function<void(const std::string&&)>, std::function<void(const std::string&&, const uint_fast32_t)>> receiveCb,
                        std::function<void(void)> connectionLostCb,
                        std::variant<std::function<void(const uint_fast32_t)>, std::function<void(const std::string&&)>, std::function<void(const std::string&&, const uint_fast32_t)>> sendFailCb,
                        uint_fast16_t sendTimeout ) : _timetoLive {sendTimeout}, _isConnected{false}, _isOpen{false}, _closeTrigger{false} {
    if(receiveCb.index() == 0) {
        if(std::get<0>(receiveCb) == nullptr){
            throw std::invalid_argument("receive callback cannot be null");    
        }
    } else {
        if(std::get<1>(receiveCb) == nullptr){
            throw std::invalid_argument("receive callback cannot be null");    
        }
    }

    _receiveCb = receiveCb;
    _connectionLostCb = connectionLostCb;
    _sendFailCb = sendFailCb;

    // todo: burada out stream alınarak log atılabilir mi?
    // finalcut logger da dependancy diye bir log sayfası olur, oraya basmak için bu kütüphane gibi dependency'lere verilen stramden gelenler o log sayfasına basılır
}

SctpClient::~SctpClient() {
    if(_isConnected){
        close();
    } else {
        if(_isOpen){
            ::close(_sockFd);
        }
    }
}

////////////////////
/////  public  /////
////////////////////

std::pair<SctpClient::ConnectionResult, std::string_view> SctpClient::connect(const std::string& address, uint16_t port)
{    
    if(_isOpen == false) 
    {       
        // open sctp socket
        _sockFd = socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
        if (_sockFd < 0) {
            // socket open occurred, return error number and message
            return std::make_pair(ConnectionResult::SocketError, std::string_view("Open failed. Error number: " + std::to_string(errno) + " Message: " + strerror(errno)));
        }

        // socket subscribe to required events
        struct sctp_event_subscribe events;
        memset(&events, 0, sizeof(events));         // clean struct
        events.sctp_association_event = 1;          // assoc down
        events.sctp_send_failure_event_event = 1;   // send failed
        if (setsockopt(_sockFd, IPPROTO_SCTP, SCTP_EVENTS, &events, sizeof(events)) < 0) {
            // subs failed, return error number and message
            ::close(_sockFd);
            return std::make_pair(ConnectionResult::SocketError, std::string_view("Socket notification subscribe failed. Error number: " + std::to_string(errno) + " Message: " + strerror(errno)));
        }

        // set socket for pr-sctp mode
        struct sctp_default_prinfo defaultPrInfo;
        memset(&defaultPrInfo, 0, sizeof(defaultPrInfo));         // clean struct
        defaultPrInfo.pr_assoc_id = SCTP_FUTURE_ASSOC;
        defaultPrInfo.pr_policy = SCTP_PR_SCTP_PRIO;
        defaultPrInfo.pr_value = _timetoLive;
        if (setsockopt(_sockFd, IPPROTO_SCTP, SCTP_DEFAULT_PRINFO, &defaultPrInfo, sizeof(defaultPrInfo)) < 0) {
            // subs failed, return error number and message
            ::close(_sockFd);
            return std::make_pair(ConnectionResult::SocketError, std::string_view("Socket pr-sctp setting failed. Error number: " + std::to_string(errno) + " Message: " + strerror(errno)));
        }

        _isOpen = true;
    }

    // check and form ip address
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));  // clean struct
    serverAddr.sin_family = AF_INET;                   // todo: ipv6 consideration ???
    serverAddr.sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &serverAddr.sin_addr) <= 0) {
        // invalid address
        return std::make_pair(ConnectionResult::SocketError, std::string_view("Invalid IP address"));
    }

    // sctp connect
    if (::connect(_sockFd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        // connect failed
        return std::make_pair(ConnectionResult::PeerConnectError, std::string_view("sctp connect failed. Error number: " + std::to_string(errno) + " Message: " + strerror(errno)));
    }

    /*
    // struct sctp_rtoinfo params;
    // socklen_t optlen = sizeof(params);
    // getsockopt(_sockFd, IPPROTO_SCTP, SCTP_RTOINFO, &params, &optlen);
    // std::cout << "rto initial: " << (unsigned int) params.srto_initial << std::endl;
    // std::cout << "rto max: " << (unsigned int) params.srto_max << std::endl;
    // std::cout << "rto min: " << (unsigned int) params.srto_min << std::endl;

    // struct sctp_assocparams params2;
    // optlen = sizeof(params2);
    // getsockopt(_sockFd, IPPROTO_SCTP, SCTP_ASSOCINFO, &params2, &optlen);
    // std::cout << "assoc max rtx: " << (unsigned int) params2.sasoc_asocmaxrxt << std::endl;
    // std::cout << "assoc max peer: " << (unsigned int) params2.sasoc_number_peer_destinations << std::endl;

    // struct sctp_prstatus params3;
    // optlen = sizeof(params3);
    // getsockopt(_sockFd, IPPROTO_SCTP, SCTP_PR_ASSOC_STATUS, &params3, &optlen);
    // std::cout << "pr-sctp policy: " << (unsigned int) params3.sprstat_policy << std::endl;

    // struct sctp_assoc_value params4;
    // optlen = sizeof(params4);
    // getsockopt(_sockFd, IPPROTO_SCTP, SCTP_PR_SUPPORTED, &params4, &optlen);
    // std::cout << "pr-supported -- assoc-id: " << (unsigned int) params4.assoc_id << std::endl;
    // std::cout << "pr-supported -- assoc value(?): " << (unsigned int) params4.assoc_value << std::endl;

    // struct sctp_default_prinfo params5;
    // optlen = sizeof(params5);
    // getsockopt(_sockFd, IPPROTO_SCTP, SCTP_DEFAULT_PRINFO, &params5, &optlen);
    // std::cout << "default pr -- assoc-id: " << (unsigned int) params5.pr_assoc_id << std::endl;
    // std::cout << "default pr -- pr-value(?): " << (unsigned int) params5.pr_value << std::endl;
    // std::cout << "default pr -- pr policy: " << (unsigned int) params5.pr_policy << std::endl;
    */

    // start receive thread and return success
    _isConnected = true;
    _receiveThread = std::thread(&SctpClient::_receiveLoop, this);

    return std::make_pair(ConnectionResult::Success, std::string_view());
}

void SctpClient::close(void)
{   
    if(_isConnected) {
        // send shutdown, join receive thread, close socket
        _isConnected = false;
        shutdown(_sockFd, SHUT_RDWR);
        if (_receiveThread.joinable()) {
            _receiveThread.join();
        }
    }

    if(_isOpen){
        _isOpen = false;
        ::close(_sockFd);
    }
}

bool SctpClient::send(const std::string& message, uint_fast32_t context) {   
    if(_isConnected == false){
        return false;
    }

    int ret = sctp_sendmsg(_sockFd, const_cast<char*>(message.data()), message.size(), nullptr, 0, 0, SCTP_PR_SCTP_PRIO, 0, _timetoLive, context);
    if(ret > 0){
        return (message.length() == (size_t) ret);
    } else {
        return false;
    }
}

bool SctpClient::addPath(const std::string& address) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, address.c_str(), &addr.sin_addr);

    if (sctp_bindx(_sockFd, (struct sockaddr*)&addr, 1, SCTP_BINDX_ADD_ADDR) == 0) {
        return true;
    } else {
        return false;
    }
}

bool SctpClient::removePath(const std::string& address) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, address.c_str(), &addr.sin_addr);

    if (sctp_bindx(_sockFd, (struct sockaddr*)&addr, 1, SCTP_BINDX_REM_ADDR) == 0) {
        return true;
    } else {
        return false;
    }
}

/////////////////////
/////  private  /////
/////////////////////

void SctpClient::_receiveLoop(void) {
    char buffer[RECV_BUFFER_SIZE];

    while(_isConnected) {
        struct sctp_sndrcvinfo sndrcvinfo;
        int flags = 0;

        // Receive messages or notifications
        int recv_len = sctp_recvmsg(_sockFd, buffer, RECV_BUFFER_SIZE, nullptr, nullptr, &sndrcvinfo, &flags);
        
        if (recv_len < 0) {
            _rcvErrCounter++;
            continue;
        }

        // Check if it's a notification
        if (flags & MSG_NOTIFICATION) {
            _handleNotification((union sctp_notification*)buffer, recv_len);    // todo: reinterpret_cast
        } else {
            // Handle regular message receive
            if(recv_len > 0){
                if(_receiveCb.index() == 0) {
                    std::get<0>(_receiveCb)(std::string(buffer, recv_len));
                } else {
                    std::get<1>(_receiveCb)(std::string(buffer, recv_len), sndrcvinfo.sinfo_context);
                }
            } else {
                _zeroLenMsgCounter++;   // received zero length msg in test when connection lost does not handled properly, so this check
            }
        }
    }

    if(_closeTrigger && (_isOpen == true)){
        _closeTrigger = false;
        _isOpen = false;
        
        ::close(_sockFd);
        if(_connectionLostCb){
            _connectionLostCb();
        }
    }
}

void SctpClient::_handleNotification(const union sctp_notification* notif, size_t notifLen) {
    if (notif == nullptr || notifLen < sizeof(((union sctp_notification*)NULL)->sn_header)) {
        _notifSizeErrCounter++;
        return;
    }

    switch (notif->sn_header.sn_type) {
        case SCTP_ASSOC_CHANGE: {
            if(notifLen < sizeof(struct sctp_assoc_change)){
                _notifSizeErrCounter++;
                break;
            }

            const struct sctp_assoc_change *assocChangeEvent = &notif->sn_assoc_change;
            switch(assocChangeEvent->sac_state) {
                case SCTP_COMM_LOST:
                case SCTP_SHUTDOWN_COMP:
                    _closeTrigger = true;   // close method should be called outside of receive loop while
                    _isConnected = false;
                    break;
                case SCTP_COMM_UP:
                    break;    
                default:
                    _unhandledAssocChgCounter++;
                    break;
            }
            break;
        }
        case SCTP_SEND_FAILED_EVENT: {
            if(notifLen < sizeof(struct sctp_send_failed_event)){
                _notifSizeErrCounter++;
                break;
            }

            const struct sctp_send_failed_event *sendFailEvent = &notif->sn_send_failed_event;
            if(_sendFailCb.index() == 0) {          // context only   
                if(std::get<0>(_sendFailCb) != nullptr){
                    std::get<0>(_sendFailCb)(sendFailEvent->ssfe_info.snd_context);
                }
            } else {
                std::string dataStr {(const char*)sendFailEvent->ssf_data, sendFailEvent->ssf_length - offsetof(sctp_send_failed_event, ssf_data)};
                if(_sendFailCb.index() == 1) {      // data only
                    std::get<1>(_sendFailCb)(std::move(dataStr));
                } else {                            // data + context
                    std::get<2>(_sendFailCb)(std::move(dataStr), sendFailEvent->ssfe_info.snd_context);
                }
            }
            break;
        }
        default:
            // cannot log, just break
            break;
    }
}
